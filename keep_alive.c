/*
 * keep_alive.c — HTTP/1.1 persistent connection loop (Week 5)
 *
 * Architecture overview:
 *
 *   handle_connection()                    ← called by worker thread
 *     │ 
 *     ├─ set SO_RCVTIMEO on socket
 *     │
 *     └─ for (;;) {                        ← per-request loop
 *           read_next_request()            ← accumulate bytes, call parser
 *           dispatch_request()             ← serve the request
 *           if (!keep_alive) break         ← Connection: close → done
 *           if (requests >= max) break     ← cap reached
 *           reset buffer for next request  ← KEY: slide leftover bytes
 *        }
 *
 * The hardest part (and the most common source of bugs in home-built
 * servers) is knowing where one HTTP request ends and the next begins
 * in the recv() buffer.
 *
 * We solve this with a "sliding window":
 *
 *   buf[0 .. total-1]   = all bytes received so far on this connection
 *
 *   After parse_http_request() returns PARSE_OK, we know exactly how
 *   many bytes the first request consumed.  We call
 *   http_request_consumed_bytes() to compute that length, then
 *   memmove the leftover bytes to the front of the buffer.  On the
 *   next iteration of the loop the next request starts cleanly.
 *
 *   This handles pipelining correctly: a client may send two requests
 *   in one TCP segment; without the sliding window, we'd lose the
 *   second one.
 *
 * SO_RCVTIMEO mechanics:
 *
 *   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))
 *
 *   After setting this, recv() returns -1 with errno == EAGAIN (Linux)
 *   or EWOULDBLOCK when the timeout fires.  We treat that as "idle
 *   connection" and close gracefully.
 *
 * RFC 7230 §6.3 notes:
 *   "A server that does not support persistent connections MUST send
 *    the 'close' connection option in every response message."
 *
 *   We send "Connection: keep-alive" or "Connection: close" on every
 *   response so the client always knows the state.
 */

#include "keep_alive.h"
#include "http_parser.h"
#include "http_response.h"
#include "file_server.h"
#include "chunked.h"
#include <stdatomic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>     /* struct timeval */

/* ------------------------------------------------------------------
 *  Internal helpers
 * ------------------------------------------------------------------ */

/*
 * set_recv_timeout — arm the socket with SO_RCVTIMEO.
 *
 * After this call, recv() will return -1 / EAGAIN if `seconds` pass
 * with no data arriving.  This gives us "free" idle timeout without
 * any extra threads or timers.
 *
 * Linux: errno == EAGAIN on timeout.
 * macOS/BSD: errno == EAGAIN or EWOULDBLOCK (both are fine to check).
 */
static void set_recv_timeout(int fd, int seconds)
{
    struct timeval tv;
    tv.tv_sec  = seconds;
    tv.tv_usec = 0;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   &tv, sizeof(tv)) < 0) {
        perror("[keep_alive] setsockopt SO_RCVTIMEO");
        /*
         * Non-fatal: the connection will still work, it just won't
         * time out on idle.  Log it and continue.
         */
    }
}

/*
 * http_request_consumed_bytes — compute how many bytes of the buffer
 * the parsed request used.
 *
 * After parse_http_request() returns PARSE_OK, we know:
 *   - Where the header section ends (header_end + 4 for \r\n\r\n)
 *   - How many body bytes were expected (req->content_length)
 *
 * So the total consumed bytes =
 *     (offset of body start from buf) + content_length
 *
 * We re-scan for \r\n\r\n here because parse_http_request() doesn't
 * expose the header_end pointer.  This is a clean way to find it.
 *
 * Returns the number of bytes consumed, or 0 on failure (caller
 * should treat the whole buffer as consumed to avoid loops).
 */
static size_t http_request_consumed_bytes(const char *buf, size_t len,
                                          const HttpRequest *req)
{
    /* Find \r\n\r\n — the end of the headers */
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i]   == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            size_t header_section_end = i + 4;   /* past \r\n\r\n */
            return header_section_end + req->content_length;
        }
    }

    /* Should not happen if parse_http_request() returned PARSE_OK */
    return len;
}

/* ------------------------------------------------------------------
 *  dispatch_request — serve one fully-parsed HTTP request
 *
 *  Returns 1 if the connection should stay open, 0 to close.
 * ------------------------------------------------------------------ */
int dispatch_request(int client_fd, const HttpRequest *req)
{
    /*
     * If the client said "Connection: close", we must honour it.
     * Even if we'd prefer to keep the connection open, RFC 7230 §6.3
     * says the client's "close" token takes effect after the response.
     *
     * We'll pass keep_alive through to the response so the correct
     * Connection header is sent, then return 0 to close after.
     */
    int keep_alive = req->keep_alive;

    /*
     * Route the request.
     *
     * For now: serve static files from DEFAULT_DOC_ROOT.
     * Week 6 will add a proper router with API endpoints.
     *
     * serve_static() sends the complete response (headers + body).
     * It also adds the Connection header via req->keep_alive.
     */
    serve_static(client_fd, req, DEFAULT_DOC_ROOT);

    /*
     * serve_static() already sent the Connection header based on
     * req->keep_alive.  Return the same value so our loop knows
     * whether to continue.
     */
    return keep_alive;
}

/* ------------------------------------------------------------------
 *  handle_connection — main entry point for one TCP connection
 * ------------------------------------------------------------------ */

/*
 * This function replaces handle_client() from Weeks 2-4.
 *
 * It is called by worker_thread() in thread_pool.c.
 * It is responsible for closing client_fd before returning.
 */
void handle_connection(int client_fd)
{
    /* ------------------------------------------------------------------
     * Step 1: Set idle timeout
     *
     * We want the connection to close if the client goes silent after
     * we've sent a response.  SO_RCVTIMEO is the cleanest way to do
     * this in a blocking-I/O model.
     * ------------------------------------------------------------------ */
    set_recv_timeout(client_fd, KEEPALIVE_TIMEOUT_SEC);

    /* ------------------------------------------------------------------
     * Step 2: Allocate the receive buffer
     *
     * This single buffer is reused across ALL requests on this
     * connection.  After each request is processed, leftover bytes
     * (the start of the next request) are slid to the front via
     * memmove() — see "sliding window" comment at top of file.
     * ------------------------------------------------------------------ */
    char   buf[KA_RECV_BUFFER_SIZE];
    size_t total    = 0;    /* bytes currently in buf */
    int    req_count = 0;   /* requests served on this connection */

    printf("[ka] connection fd=%d  timeout=%ds  max_req=%d\n",
           client_fd, KEEPALIVE_TIMEOUT_SEC, KEEPALIVE_MAX_REQUESTS);

    /* ------------------------------------------------------------------
     * Step 3: Per-request loop
     * ------------------------------------------------------------------ */
    for (;;) {

        /* ---- 3a. Accumulate bytes until we have a full request ---- */

        HttpRequest req;
        ParseResult result = PARSE_INCOMPLETE;

        while (result == PARSE_INCOMPLETE) {

            if (total >= sizeof(buf) - 1) {
                /*
                 * Buffer full and still no complete request.
                 * The client is either very slow or malicious.
                 * Send 413 and kill the connection.
                 */
                printf("[ka] fd=%d request too large — closing\n", client_fd);
                http_response_error(client_fd, 413, "Request Entity Too Large");
                goto close_conn;
            }

            ssize_t n = recv(client_fd,
                             buf + total,
                             sizeof(buf) - 1 - total,
                             0);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /*
                     * SO_RCVTIMEO fired — the connection has been idle
                     * for KEEPALIVE_TIMEOUT_SEC seconds.
                     *
                     * If we haven't started a request yet (total == 0),
                     * this is a normal idle timeout — close silently.
                     * If we're mid-request, the client stalled — also close.
                     */
                    printf("[ka] fd=%d idle timeout after %d requests\n",
                           client_fd, req_count);
                    goto close_conn;
                }

                if (errno == EINTR) continue;   /* signal — retry */

                perror("[ka] recv");
                goto close_conn;
            }

            if (n == 0) {
                /*
                 * Client closed the connection (TCP FIN received).
                 * This is normal — browser closes after the last resource.
                 */
                printf("[ka] fd=%d client closed gracefully after %d req\n",
                       client_fd, req_count);
                goto close_conn;
            }

            total      += (size_t)n;
            buf[total]  = '\0';

            /* Try to parse whatever we have so far */
            result = parse_http_request(buf, total, &req);

            if (result == PARSE_ERROR) {
                printf("[ka] fd=%d parse error — sending 400\n", client_fd);
                http_response_error(client_fd, 400, "Bad Request");
                goto close_conn;
            }

            /* PARSE_INCOMPLETE → loop back and recv() more bytes */
        }

        /* result == PARSE_OK here */
        req_count++;
        printf("[ka] fd=%d request #%d  %s %s  keep=%s\n",
               client_fd, req_count,
               req.method, req.path,
               req.keep_alive ? "yes" : "no");

        /* ---- 3b. Dispatch the request and send the response ---- */
        int keep_alive = dispatch_request(client_fd, &req);

        /* ---- 3c. Check whether to close ---- */

        if (!keep_alive) {
            printf("[ka] fd=%d Connection: close — ending after %d req\n",
                   client_fd, req_count);
            goto close_conn;
        }

        if (KEEPALIVE_MAX_REQUESTS > 0 &&
            req_count >= KEEPALIVE_MAX_REQUESTS) {
            printf("[ka] fd=%d max requests (%d) reached — closing\n",
                   client_fd, KEEPALIVE_MAX_REQUESTS);
            goto close_conn;
        }

        /* ---- 3d. Sliding window — prepare buffer for next request ---- */

        /*
         * Compute how many bytes the request we just served consumed.
         * Any bytes AFTER that are the beginning of the NEXT request
         * (pipelining: the client may have sent request N+1 before
         * receiving our response to request N).
         *
         * We memmove those leftover bytes to the start of buf so the
         * next iteration can find \r\n\r\n at the correct offset.
         *
         * If there are no leftover bytes (total == consumed), the
         * buffer is clean and we just reset total = 0.
         *
         * This is the KEY operation for correct pipelining.
         */
        size_t consumed = http_request_consumed_bytes(buf, total, &req);

        if (consumed >= total) {
            /* Common case: buffer exactly consumed */
            total = 0;
        } else {
            /* Pipelining: slide leftover bytes to front */
            size_t leftover = total - consumed;
            memmove(buf, buf + consumed, leftover);
            total = leftover;
            printf("[ka] fd=%d pipelining: %zu leftover bytes\n",
                   client_fd, leftover);
        }

        /*
         * Re-arm the recv timeout for the next request.
         * SO_RCVTIMEO is set per-socket and doesn't need re-arming,
         * but we print a debug note here to make the flow clear.
         */
    }   /* end for(;;) */

close_conn:
    /*
     * We own the fd lifecycle.  Close it here, not in the worker thread.
     * The worker thread must NOT close fd after handle_connection() returns.
     */
    close(client_fd);
    printf("[ka] fd=%d closed  (served %d requests)\n", client_fd, req_count);
}