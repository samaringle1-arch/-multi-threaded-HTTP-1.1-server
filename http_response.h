/*
 * http_response.h — HTTP/1.1 response builder (Week 4)
 *
 * Separates response *construction* from response *sending* so each
 * part is independently testable and reusable.
 *
 * Typical usage — serving a file:
 *
 *   HttpResponse res;
 *   http_response_init(&res, 200, "OK");
 *   http_response_add_header(&res, "Content-Type",  "text/html");
 *   http_response_add_header(&res, "Content-Length", "1024");
 *   http_response_send_headers(client_fd, &res);
 *   http_response_sendfile(client_fd, file_fd, file_size);
 *
 * Typical usage — sending an error:
 *
 *   http_response_error(client_fd, 404, "Not Found");
 */

#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>
#include <sys/types.h>     /* off_t */

/* ------------------------------------------------------------------ */
/*  Limits                                                             */
/* ------------------------------------------------------------------ */
#define MAX_RESP_HEADERS      24
#define RESP_HEADER_NAME_MAX  64
#define RESP_HEADER_VAL_MAX   256

/* ------------------------------------------------------------------ */
/*  HttpResponse                                                       */
/*                                                                     */
/*  Holds a status line and a set of header fields.                   */
/*  Does NOT hold the body — the body is written separately via       */
/*  http_response_send_body() or http_response_sendfile().            */
/* ------------------------------------------------------------------ */
typedef struct {
    int  status;
    char reason[64];

    struct {
        char name [RESP_HEADER_NAME_MAX];
        char value[RESP_HEADER_VAL_MAX];
    } headers[MAX_RESP_HEADERS];

    int header_count;
} HttpResponse;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Initialise a response with a status code and reason phrase */
void http_response_init(HttpResponse *res, int status, const char *reason);

/* Add a "Name: Value" header field */
void http_response_add_header(HttpResponse *res,
                              const char *name, const char *value);

/* Convenience: add Content-Length as a formatted integer */
void http_response_set_content_length(HttpResponse *res, off_t length);

/*
 * Serialise and transmit: status line + all headers + blank line.
 * Call this once, then call one of the body-sending functions below.
 */
int http_response_send_headers(int fd, const HttpResponse *res);

/*
 * Send a buffer as the response body.
 * Must be called AFTER http_response_send_headers().
 */
int http_response_send_body(int fd, const char *body, size_t len);

/*
 * Zero-copy file transfer using sendfile(2).
 * Must be called AFTER http_response_send_headers().
 *
 * sendfile() transfers directly from the file's page cache to the
 * socket buffer — no data ever enters userspace. On a warm cache this
 * is 3-5× faster than read()+write() for static file serving.
 */
int http_response_sendfile(int client_fd, int file_fd, off_t file_size);

/*
 * Build and send a complete error response in one call.
 * Generates a tiny HTML body so browsers render something readable.
 *
 *   http_response_error(fd, 404, "Not Found");
 *   http_response_error(fd, 405, "Method Not Allowed");
 */
void http_response_error(int fd, int status, const char *reason);

/* Format the current UTC time as an HTTP-date string */
void http_date_now(char *buf, size_t size);

#endif /* HTTP_RESPONSE_H */