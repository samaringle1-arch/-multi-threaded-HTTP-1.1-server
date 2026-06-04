/*
 * chunked.c — HTTP/1.1 chunked Transfer-Encoding implementation
 *
 * RFC 7230 §4.1 wire format:
 *
 *   chunked-body = *chunk last-chunk trailer-part CRLF
 *   chunk        = chunk-size CRLF chunk-data CRLF
 *   chunk-size   = 1*HEXDIG
 *   last-chunk   = 1*("0") CRLF
 *   trailer-part = *( header-field CRLF )   ← we always omit this
 *
 * A minimal valid chunked body for the string "Hello, World!" (13 bytes):
 *
 *   d\r\n
 *   Hello, World!\r\n
 *   0\r\n
 *   \r\n
 *
 * Note: 0xD == 13 decimal.
 */

#include "chunked.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>

/* ------------------------------------------------------------------
 *  Internal helper: write all bytes, retrying on EINTR
 * ------------------------------------------------------------------ */

/*
 * write_all — keep calling write() until all `len` bytes are sent
 * or a real error occurs.
 *
 * Why not just write() once?
 * write() on a socket can return fewer bytes than requested
 * (a "short write") if the send buffer is full.  This is normal
 * and not an error.  We must loop until done.
 *
 * Returns 0 on success, -1 on error (errno is set by write()).
 */
static int write_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);

        if (n < 0) {
            if (errno == EINTR) continue;   /* signal interrupted — retry */
            return -1;                      /* real error */
        }

        if (n == 0) return -1;             /* fd closed unexpectedly */

        sent += (size_t)n;
    }

    return 0;
}

/* ------------------------------------------------------------------
 *  chunked_send_body — one-shot API
 * ------------------------------------------------------------------ */

/*
 * Send `len` bytes as a SINGLE chunk followed by the terminal 0-chunk.
 *
 * Wire bytes sent (example: data = "Hello" (5 bytes)):
 *
 *   5\r\n
 *   Hello\r\n
 *   0\r\n
 *   \r\n
 *
 * This is the simplest possible chunked body and is correct per RFC.
 */
ssize_t chunked_send_body(int fd, const char *data, size_t len)
{
    if (len == 0) {
        /*
         * Caller has no body at all — just send the terminal chunk.
         * This is valid and tells the client the response body is empty.
         */
        const char terminal[] = "0\r\n\r\n";
        if (write_all(fd, terminal, sizeof(terminal) - 1) < 0)
            return -1;
        return 0;
    }

    /*
     * Step 1: chunk-size line
     *
     * The size is written in hexadecimal, followed by \r\n.
     * snprintf with %zx gives us the lowercase hex representation.
     *
     * Max size_t in hex is 16 chars; +3 for "\r\n\0" = 19 bytes total.
     */
    char size_line[24];
    int  size_line_len = snprintf(size_line, sizeof(size_line),
                                  "%zx\r\n", len);
    if (size_line_len < 0) return -1;

    if (write_all(fd, size_line, (size_t)size_line_len) < 0) return -1;

    /* Step 2: chunk-data */
    if (write_all(fd, data, len) < 0) return -1;

    /* Step 3: CRLF after chunk-data */
    if (write_all(fd, "\r\n", 2) < 0) return -1;

    /*
     * Step 4: terminal chunk
     *
     * "0\r\n\r\n" — chunk size 0, followed by the mandatory
     * trailing CRLF that ends the chunked body.
     */
    if (write_all(fd, "0\r\n\r\n", 5) < 0) return -1;

    return (ssize_t)len;
}

/* ------------------------------------------------------------------
 *  ChunkedWriter — streaming API
 * ------------------------------------------------------------------ */

void chunked_writer_init(ChunkedWriter *cw, int fd)
{
    cw->fd         = fd;
    cw->bytes_sent = 0;
    cw->error      = 0;
}

/*
 * chunked_writer_send — send one chunk.
 *
 * Each call sends exactly:
 *   <hex-size>\r\n
 *   <data>\r\n
 *
 * Callers can call this as many times as needed.  Empty chunks
 * (len == 0) are skipped — they would look like the terminal chunk.
 */
int chunked_writer_send(ChunkedWriter *cw, const char *data, size_t len)
{
    /* Propagate any earlier error — once broken, stay broken. */
    if (cw->error) return -1;

    /*
     * Skip zero-length chunks.
     *
     * RFC 7230 §4.1 says a chunk-size of 0 signals end-of-body
     * (the terminal chunk).  We must never accidentally send a
     * 0-size chunk in the middle of a streaming response.
     */
    if (len == 0) return 0;

    /* --- chunk-size line --- */
    char size_line[24];
    int  size_line_len = snprintf(size_line, sizeof(size_line),
                                  "%zx\r\n", len);
    if (size_line_len < 0) { cw->error = 1; return -1; }

    if (write_all(cw->fd, size_line, (size_t)size_line_len) < 0) {
        cw->error = 1;
        return -1;
    }

    /* --- chunk-data --- */
    if (write_all(cw->fd, data, len) < 0) {
        cw->error = 1;
        return -1;
    }

    /* --- CRLF after data --- */
    if (write_all(cw->fd, "\r\n", 2) < 0) {
        cw->error = 1;
        return -1;
    }

    cw->bytes_sent += len;
    return 0;
}

/*
 * chunked_writer_finish — send the terminal 0-chunk.
 *
 * Must be called exactly once after all chunked_writer_send() calls.
 *
 * Wire bytes:
 *   0\r\n
 *   \r\n
 *
 * After this the HTTP message is complete.  If the connection is
 * keep-alive, the server can immediately read the next request.
 */
int chunked_writer_finish(ChunkedWriter *cw)
{
    if (cw->error) return -1;

    if (write_all(cw->fd, "0\r\n\r\n", 5) < 0) {
        cw->error = 1;
        return -1;
    }

    return 0;
}