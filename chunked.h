/*
 * chunked.h — HTTP/1.1 chunked Transfer-Encoding (Week 5)
 *
 * What is chunked encoding?
 *
 *   Normally, a server sends Content-Length so the client knows
 *   when the body ends.  But sometimes the total length is unknown
 *   when you start sending — e.g. dynamically generated pages,
 *   streaming data, or when compressing on the fly.
 *
 *   Chunked encoding solves this without buffering the whole body:
 *
 *     HTTP/1.1 200 OK\r\n
 *     Transfer-Encoding: chunked\r\n
 *     \r\n
 *     1a\r\n           ← chunk size in hex (26 decimal)
 *     abcdefghijklmnopqrstuvwxyz\r\n
 *     5\r\n            ← another chunk (5 bytes)
 *     hello\r\n
 *     0\r\n            ← terminal chunk (size == 0 means "done")
 *     \r\n             ← trailing CRLF ends the chunked body
 *
 *   RFC 7230 §4.1 specifies this format exactly.
 *
 * When to use chunked vs Content-Length:
 *
 *   Content-Length  — preferred when size is known (file serving).
 *                     More efficient because no chunk overhead.
 *   chunked         — required when size is unknown at response start
 *                     (directory listings, API responses built on-the-fly,
 *                     server-sent events, streaming).
 *                     Also mandatory if you want to use keep-alive
 *                     without buffering the whole body first.
 *
 * This module provides two interfaces:
 *
 *   1. chunked_send_body()  — one-shot: wrap an existing buffer in chunks.
 *      Simplest: caller still has the whole body in memory, but wants
 *      to avoid sending Content-Length (e.g. length computed late).
 *
 *   2. chunked_writer_*()   — streaming: send pieces incrementally.
 *      No buffer needed.  Write chunks as you generate them.
 *
 * RFC reference: RFC 7230 §4.1
 */

#ifndef CHUNKED_H
#define CHUNKED_H

#include <stddef.h>
#include <sys/types.h>

/* ------------------------------------------------------------------
 *  One-shot API
 * ------------------------------------------------------------------ */

/*
 * chunked_send_body — send `len` bytes of `data` as one chunk,
 * followed by the terminal 0-chunk.
 *
 * Use this when you have the full body in memory and just want
 * Transfer-Encoding: chunked semantics (e.g. no Content-Length).
 *
 * Precondition: caller must have already sent headers with
 *   Transfer-Encoding: chunked  (NOT Content-Length).
 *
 * Returns bytes written on success, -1 on error.
 */
ssize_t chunked_send_body(int fd, const char *data, size_t len);

/* ------------------------------------------------------------------
 *  Streaming (incremental) API
 * ------------------------------------------------------------------ */

/*
 * ChunkedWriter — state for an in-progress chunked response.
 *
 * Typical usage:
 *
 *   ChunkedWriter cw;
 *   chunked_writer_init(&cw, client_fd);
 *
 *   chunked_writer_send(&cw, part1, len1);
 *   chunked_writer_send(&cw, part2, len2);
 *   ...
 *   chunked_writer_finish(&cw);   // sends terminal 0-chunk
 */
typedef struct {
    int    fd;           /* socket to write to */
    size_t bytes_sent;   /* total data bytes sent (excludes framing) */
    int    error;        /* set to 1 if any write failed */
} ChunkedWriter;

/*
 * chunked_writer_init — attach a writer to a socket fd.
 * Headers must already be sent with Transfer-Encoding: chunked.
 */
void chunked_writer_init(ChunkedWriter *cw, int fd);

/*
 * chunked_writer_send — send one chunk of `len` bytes from `data`.
 *
 * Formats and writes:
 *   <hex-length>\r\n
 *   <data>\r\n
 *
 * Sets cw->error on failure.  Subsequent calls become no-ops.
 * Returns 0 on success, -1 on error.
 */
int chunked_writer_send(ChunkedWriter *cw, const char *data, size_t len);

/*
 * chunked_writer_finish — send the terminal zero-chunk.
 *
 * Writes:
 *   0\r\n
 *   \r\n
 *
 * Must be called exactly once, at the end of the response body.
 * After this, the connection may be reused (if keep-alive).
 * Returns 0 on success, -1 on error.
 */
int chunked_writer_finish(ChunkedWriter *cw);

/* ------------------------------------------------------------------
 *  Response header helper
 * ------------------------------------------------------------------ */

/*
 * http_response_set_chunked — add Transfer-Encoding: chunked to res.
 *
 * This is a convenience wrapper over http_response_add_header().
 * It also ensures Content-Length is NOT present (they are mutually
 * exclusive per RFC 7230 §3.3).
 */
struct HttpResponse;   /* forward-declared; defined in http_response.h */

#endif /* CHUNKED_H */