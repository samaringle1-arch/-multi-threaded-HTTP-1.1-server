/*
 * http_parser.h — HTTP/1.1 request parser (Week 2)
 *
 * Design principle: the parser is a PURE FUNCTION.
 * It takes a raw byte buffer and a length.
 * It knows nothing about sockets, file descriptors, or threads.
 * This makes it independently testable — see test_parser.c.
 *
 * Usage pattern (in handle_client):
 *
 *   char buf[RECV_BUFFER_SIZE];
 *   size_t total = 0;
 *
 *   while (total < sizeof(buf)) {
 *       ssize_t n = recv(fd, buf + total, sizeof(buf) - total, 0);
 *       if (n <= 0) break;
 *       total += n;
 *
 *       HttpRequest req;
 *       ParseResult r = parse_http_request(buf, total, &req);
 *       if (r == PARSE_OK)         { handle it; break; }
 *       if (r == PARSE_ERROR)      { send 400; break;  }
 *       // PARSE_INCOMPLETE → keep reading
 *   }
 */

#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  Limits                                                             */
/*  Sized to cover any realistic HTTP/1.1 request.                    */
/* ------------------------------------------------------------------ */
#define MAX_METHOD_LEN    16      /* "DELETE" is 6, leave room */
#define MAX_PATH_LEN      2048    /* URI path, matches most servers */
#define MAX_VERSION_LEN   16      /* "HTTP/1.1" is 8 */
#define MAX_HEADER_NAME   128
#define MAX_HEADER_VALUE  4096    /* Cookie headers can be large */
#define MAX_HEADERS       32      /* Enough for any real browser */

/* ------------------------------------------------------------------ */
/*  ParseResult                                                        */
/*                                                                     */
/*  Three outcomes are possible every time you call parse_http_request */
/* ------------------------------------------------------------------ */
typedef enum {
    PARSE_OK         = 0,  /* Request fully and correctly parsed       */
    PARSE_INCOMPLETE = 1,  /* \r\n\r\n not yet seen — read more bytes  */
    PARSE_ERROR      = 2   /* Malformed request — send 400             */
} ParseResult;

/* ------------------------------------------------------------------ */
/*  HttpHeader — one "Name: Value" header field                        */
/* ------------------------------------------------------------------ */
typedef struct {
    char name [MAX_HEADER_NAME];   /* e.g. "Content-Type"        */
    char value[MAX_HEADER_VALUE];  /* e.g. "application/json"    */
} HttpHeader;

/* ------------------------------------------------------------------ */
/*  HttpRequest — a fully parsed HTTP/1.1 request                     */
/* ------------------------------------------------------------------ */
typedef struct {
    /* Request line */
    char method [MAX_METHOD_LEN];   /* GET, POST, PUT, DELETE, HEAD  */
    char path   [MAX_PATH_LEN];     /* /api/users                    */
    char query  [MAX_PATH_LEN];     /* id=42&sort=asc  (after '?')   */
    char version[MAX_VERSION_LEN];  /* HTTP/1.0 or HTTP/1.1          */

    /* Headers */
    HttpHeader headers[MAX_HEADERS];
    int        header_count;

    /*
     * Body — points INSIDE the original raw buffer.
     * Do NOT free this pointer. It is only valid while
     * the raw buffer is alive.
     */
    const char *body;
    size_t      body_length;

    /*
     * Derived convenience fields — computed during parse
     * so callers don't have to look up headers manually.
     */
    int    keep_alive;      /* 1 = Connection: keep-alive, 0 = close */
    size_t content_length;  /* Value of Content-Length, or 0         */
} HttpRequest;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Zero-initialise a request before parsing */
void http_request_init(HttpRequest *req);

/*
 * Main parser — call this after every recv().
 * Fills *req on PARSE_OK.  *req is undefined on any other result.
 */
ParseResult parse_http_request(const char *raw, size_t len,
                               HttpRequest *req);

/*
 * Look up a header value by name — case-insensitive.
 * Returns NULL if the header is not present.
 *
 *   const char *ct = http_get_header(&req, "content-type");
 */
const char *http_get_header(const HttpRequest *req, const char *name);

/* Debug helper — dump every field to stdout */
void http_request_print(const HttpRequest *req);

/* ------------------------------------------------------------------ */
/*  Internal helpers — exposed here so test_parser.c can unit-test    */
/* ------------------------------------------------------------------ */
ParseResult parse_request_line(const char *line, size_t len,
                               HttpRequest *req);
ParseResult parse_header_line (const char *line, size_t len,
                               HttpRequest *req);

#endif /* HTTP_PARSER_H */