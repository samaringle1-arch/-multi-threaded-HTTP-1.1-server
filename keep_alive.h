/*
 * keep_alive.h — HTTP/1.1 persistent connection management (Week 5)
 *
 *
 *   This module owns the per-connection request loop:
 *
 *     while (connection is alive) {
 *         read next request  (accumulate partial reads)
 *         parse it
 *         dispatch it
 *         send response with correct Connection header
 *         if (Connection: close)  break;
 *         if (idle > KEEPALIVE_TIMEOUT_SEC)  break;
 *     }
 *
 * Idle timeout implementation:
 *
 *   We use SO_RCVTIMEO on the socket.  After setting it, recv() returns
 *   -1 with errno == EAGAIN / EWOULDBLOCK when the socket has been idle
 *   for KEEPALIVE_TIMEOUT_SEC seconds.  We then close gracefully.
 *
 *   This is simpler than a separate timer thread and correct for a
 *   single-connection loop.  epoll-based servers (Week 8) use a different
 *   approach (timeout wheel), but SO_RCVTIMEO is perfect here.
 *
 * RFC references:
 *   RFC 7230 §6.3  — Persistence
 *   RFC 7230 §6.3.2 — Pipelining
 */

#ifndef KEEP_ALIVE_H
#define KEEP_ALIVE_H

#include "http_parser.h"

/* ------------------------------------------------------------------
 *  Tuning constants
 * ------------------------------------------------------------------ */

/*
 * How long to wait for the NEXT request on a keep-alive connection.
 *
 * After sending a response, we arm the socket with this timeout.
 * If no new request bytes arrive within this window, we close.
 *
 * 30 s matches Apache's KeepAliveTimeout default.
 */
#define KEEPALIVE_TIMEOUT_SEC   30

/*
 * How many requests we will serve on a single connection.
 *
 * Caps runaway clients and ensures a busy server recycles fds.
 * Browsers rarely send more than a few hundred requests per connection.
 * Set to 0 to disable the cap (unlimited, not recommended).
 */
#define KEEPALIVE_MAX_REQUESTS  1000

/*
 * Per-request recv buffer.  Must comfortably hold one full HTTP request
 * including all headers.  Bodies larger than this are read in pieces
 */
#define KA_RECV_BUFFER_SIZE     65536   /* 64 KB */

/* ------------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------------ */

/*
 * handle_connection — the Week 5 replacement for handle_client().
 *
 * Owns the full lifecycle of one TCP connection:
 *   - Sets SO_RCVTIMEO on the socket
 *   - Loops reading requests until close / timeout / error / max-req
 *   - Calls dispatch_request() for each complete request
 *   - Sends the right Connection header on each response
 *   - Closes the fd before returning (caller must NOT close it)
 *
 * Called by thread_pool worker_thread() instead of handle_client().
 */
void handle_connection(int client_fd);

/*
 * dispatch_request — called once per fully-parsed request.
 *
 * Decides what to do with the request (static file, echo, error)
 * and sends the response.  Returns 1 if the connection should stay
 * open, 0 if it should close (i.e. the response included
 * "Connection: close").
 */
int dispatch_request(int client_fd, const HttpRequest *req);

#endif 