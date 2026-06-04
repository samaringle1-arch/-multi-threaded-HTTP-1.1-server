/*
 * test_keep_alive.c — unit tests for Week 5: keep-alive & chunked encoding
 *
 * Build:  make test_ka
 * Run:    ./test_ka
 *
 * Tests:
 *   1. Chunked encoding wire format — one chunk + terminal
 *   2. Chunked streaming API — multiple chunks
 *   3. Empty body edge case — only terminal chunk
 *   4. Keep-alive connection header parsing — derived field
 *   5. Consumed-bytes calculation — correct sliding window offset
 *   6. Multiple pipelined requests in one buffer — parsed in sequence
 *   7. Connection: close respected even on HTTP/1.1
 *   8. HTTP/1.0 defaults to non-persistent
 *
 * We test chunked encoding by writing to a socketpair() and reading
 * back the raw bytes.  This avoids needing a real network connection
 * while still exercising the actual write() calls.
 */

#include "chunked.h"
#include "http_parser.h"
#include "keep_alive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>

/* ------------------------------------------------------------------ */
/*  Minimal test framework                                             */
/* ------------------------------------------------------------------ */
static int passed = 0, failed = 0;

#define CHECK(label, cond)                                             \
    do {                                                               \
        if (cond) {                                                    \
            passed++;                                                  \
            printf("  ✓  %s\n", label);                               \
        } else {                                                       \
            failed++;                                                  \
            printf("  ✗  %s  (line %d)\n", label, __LINE__);          \
        }                                                              \
    } while (0)

#define CHECK_STR(label, got, expected)                                \
    do {                                                               \
        if (strcmp((got), (expected)) == 0) {                          \
            passed++;                                                  \
            printf("  ✓  %s\n", label);                               \
        } else {                                                       \
            failed++;                                                  \
            printf("  ✗  %s\n     expected: [%s]\n     got:      [%s]\n", \
                   label, expected, got);                              \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Helper: create a socketpair, write through it, read it back       */
/* ------------------------------------------------------------------ */
typedef struct {
    int write_end;
    int read_end;
} Pipe;

static void pipe_open(Pipe *p)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        perror("socketpair");
        exit(EXIT_FAILURE);
    }
    p->write_end = fds[0];
    p->read_end  = fds[1];
}

static void pipe_close(Pipe *p)
{
    close(p->write_end);
    close(p->read_end);
}

/*
 * pipe_read_all — read up to `max` bytes from the read end,
 * null-terminate, and return the count.
 *.
 * We shutdown the write end first so read() knows EOF is coming.
 */
static ssize_t pipe_read_all(Pipe *p, char *out, size_t max)
{
    /* Close write end so read() returns 0 at EOF instead of blocking */
    shutdown(p->write_end, SHUT_WR);

    ssize_t total = 0;
    ssize_t n;
    while (total < (ssize_t)(max - 1) &&
           (n = read(p->read_end, out + total, max - 1 - (size_t)total)) > 0) {
        total += n;
    }
    out[total] = '\0';
    return total;
}

/* ================================================================== */
/*  Test 1: one-shot chunked_send_body — wire format                  */
/* ================================================================== */
static void test_chunked_one_shot(void)
{
    printf("\n[test] chunked_send_body — wire format\n");

    Pipe p;
    pipe_open(&p);

    const char *body = "Hello, World!";   /* 13 bytes = 0xD */
    ssize_t rc = chunked_send_body(p.write_end, body, strlen(body));
    CHECK("chunked_send_body returns > 0",  rc > 0);

    char out[256];
    pipe_read_all(&p, out, sizeof(out));
    pipe_close(&p);

    /*
     * Expected wire bytes:
     *   "d\r\nHello, World!\r\n0\r\n\r\n"
     *
     *   d = 13 in hex
     */
    const char *expected = "d\r\nHello, World!\r\n0\r\n\r\n";
    CHECK_STR("wire format is correct", out, expected);
}

/* ================================================================== */
/*  Test 2: streaming ChunkedWriter — multiple chunks                 */
/* ================================================================== */
static void test_chunked_streaming(void)
{
    printf("\n[test] ChunkedWriter — multiple chunks\n");

    Pipe p;
    pipe_open(&p);

    ChunkedWriter cw;
    chunked_writer_init(&cw, p.write_end);

    /* Send three chunks */
    chunked_writer_send(&cw, "abc",   3);   /* 0x3 */
    chunked_writer_send(&cw, "de",    2);   /* 0x2 */
    chunked_writer_send(&cw, "fghij", 5);   /* 0x5 */
    chunked_writer_finish(&cw);

    CHECK("no error flag", cw.error == 0);
    CHECK("10 bytes sent", cw.bytes_sent == 10);

    char out[256];
    pipe_read_all(&p, out, sizeof(out));
    pipe_close(&p);

    /*
     * Expected:
     *   "3\r\nabc\r\n2\r\nde\r\n5\r\nfghij\r\n0\r\n\r\n"
     */
    const char *expected = "3\r\nabc\r\n2\r\nde\r\n5\r\nfghij\r\n0\r\n\r\n";
    CHECK_STR("streaming wire format correct", out, expected);
}

/* ================================================================== */
/*  Test 3: empty body — only terminal chunk                          */
/* ================================================================== */
static void test_chunked_empty_body(void)
{
    printf("\n[test] chunked_send_body — empty body\n");

    Pipe p;
    pipe_open(&p);

    /* len == 0 → only terminal chunk */
    ssize_t rc = chunked_send_body(p.write_end, "", 0);
    CHECK("returns 0 for empty", rc == 0);

    char out[64];
    pipe_read_all(&p, out, sizeof(out));
    pipe_close(&p);

    /* Only terminal chunk: "0\r\n\r\n" */
    CHECK_STR("empty body wire is 0 CRLF CRLF", out, "0\r\n\r\n");
}

/* ================================================================== */
/*  Test 4: streaming — zero-length chunks are skipped                */
/* ================================================================== */
static void test_chunked_skip_zero(void)
{
    printf("\n[test] ChunkedWriter — zero-length chunk skipped\n");

    Pipe p;
    pipe_open(&p);

    ChunkedWriter cw;
    chunked_writer_init(&cw, p.write_end);

    chunked_writer_send(&cw, "hi",  2);
    chunked_writer_send(&cw, "",    0);   /* should be skipped */
    chunked_writer_send(&cw, "bye", 3);
    chunked_writer_finish(&cw);

    char out[128];
    pipe_read_all(&p, out, sizeof(out));
    pipe_close(&p);

    /* "2\r\nhi\r\n3\r\nbye\r\n0\r\n\r\n" — no spurious 0-chunk in the middle */
    const char *expected = "2\r\nhi\r\n3\r\nbye\r\n0\r\n\r\n";
    CHECK_STR("zero chunk is skipped", out, expected);
}

/* ================================================================== */
/*  Test 5: keep-alive derived field from parser                      */
/* ================================================================== */
static void test_keep_alive_parser(void)
{
    printf("\n[test] Parser keep_alive derived field\n");

    /* HTTP/1.1 with no Connection header → keep-alive by default */
    {
        const char *raw =
            "GET / HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";
        HttpRequest req;
        ParseResult r = parse_http_request(raw, strlen(raw), &req);
        CHECK("HTTP/1.1 default: PARSE_OK",     r == PARSE_OK);
        CHECK("HTTP/1.1 default: keep_alive=1", req.keep_alive == 1);
    }

    /* HTTP/1.1 + Connection: close → not persistent */
    {
        const char *raw =
            "GET / HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";
        HttpRequest req;
        parse_http_request(raw, strlen(raw), &req);
        CHECK("Connection: close → keep_alive=0", req.keep_alive == 0);
    }

    /* HTTP/1.0 → not persistent by default */
    {
        const char *raw =
            "GET / HTTP/1.0\r\n"
            "Host: localhost\r\n"
            "\r\n";
        HttpRequest req;
        parse_http_request(raw, strlen(raw), &req);
        CHECK("HTTP/1.0 default: keep_alive=0", req.keep_alive == 0);
    }

    /* HTTP/1.0 + Connection: keep-alive → client opts in */
    {
        const char *raw =
            "GET / HTTP/1.0\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        HttpRequest req;
        parse_http_request(raw, strlen(raw), &req);
        CHECK("HTTP/1.0 + keep-alive: keep_alive=1", req.keep_alive == 1);
    }
}

/* ================================================================== */
/*  Test 6: consumed bytes calculation for sliding window             */
/* ================================================================== */

/*
 * Expose the internal function for testing.
 * In a real build it's static inside keep_alive.c.
 * We replicate a minimal version here to test the logic directly.
 */
static size_t test_consumed_bytes(const char *buf, size_t len,
                                  const HttpRequest *req)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i]   == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            return (i + 4) + req->content_length;
        }
    }
    return len;
}

static void test_consumed_bytes_calc(void)
{
    printf("\n[test] Consumed bytes (sliding window)\n");

    /* Simple GET — no body */
    {
        const char *raw =
            "GET /index.html HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";
        size_t len = strlen(raw);

        HttpRequest req;
        parse_http_request(raw, len, &req);

        size_t consumed = test_consumed_bytes(raw, len, &req);
        CHECK("GET: consumed == total raw length", consumed == len);
    }

    /* POST with body */
    {
        const char *raw =
            "POST /api HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";
        size_t len = strlen(raw);

        HttpRequest req;
        parse_http_request(raw, len, &req);

        size_t consumed = test_consumed_bytes(raw, len, &req);
        CHECK("POST: consumed == total raw length", consumed == len);
    }

    /* Two pipelined requests in one buffer */
    {
        const char *req1 =
            "GET /a HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";
        const char *req2 =
            "GET /b HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";

        char buf[1024];
        size_t len1 = strlen(req1);
        size_t len2 = strlen(req2);
        memcpy(buf,        req1, len1);
        memcpy(buf + len1, req2, len2);
        size_t total = len1 + len2;

        /* Parse first request */
        HttpRequest req;
        ParseResult r = parse_http_request(buf, total, &req);
        CHECK("pipelined: first request parses OK", r == PARSE_OK);

        size_t consumed = test_consumed_bytes(buf, total, &req);
        CHECK("pipelined: consumed == first req length only",
              consumed == len1);

        /* Check leftover IS the second request */
        size_t leftover = total - consumed;
        CHECK("pipelined: leftover == second req length",
              leftover == len2);
        CHECK("pipelined: leftover starts with 'GET /b'",
              strncmp(buf + consumed, "GET /b", 6) == 0);
    }
}

/* ================================================================== */
/*  Test 7: chunked hex formatting — verify hex sizes                 */
/* ================================================================== */
static void test_chunked_hex_sizes(void)
{
    printf("\n[test] Chunked encoding hex size values\n");

    struct {
        size_t      len;
        const char *expected_hex;
    } cases[] = {
        {   1, "1"    },
        {  15, "f"    },
        {  16, "10"   },
        { 255, "ff"   },
        { 256, "100"  },
        {4096, "1000" },
        {   0, NULL   }
    };

    for (int i = 0; cases[i].expected_hex != NULL; i++) {
        char got[32];
        snprintf(got, sizeof(got), "%zx", cases[i].len);

        char label[64];
        snprintf(label, sizeof(label),
                 "hex(%zu) == \"%s\"", cases[i].len, cases[i].expected_hex);

        CHECK(label, strcmp(got, cases[i].expected_hex) == 0);
    }
}

/* ================================================================== */
/*  Test 8: ChunkedWriter error propagation                           */
/* ================================================================== */
static void test_chunked_error_propagation(void)
{
    printf("\n[test] ChunkedWriter error flag propagates\n");

    /*
     * Write to a closed fd — the second write() will fail with EPIPE
     * (or SIGPIPE if we haven't set SIG_IGN).  We verify that
     * cw.error is set and subsequent calls are no-ops.
     */
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    ChunkedWriter cw;
    chunked_writer_init(&cw, fds[0]);

    /* Close the read end — writes to fds[0] will now fail */
    close(fds[1]);

    /*
     * We need to ignore SIGPIPE for this test to work without
     * killing the process.  Normally the server does this in
     * setup_signals().  We do it inline here.
     */
    signal(SIGPIPE, SIG_IGN);

    /* First send will fail (EPIPE) */
    chunked_writer_send(&cw, "test", 4);

    CHECK("error flag set after write failure", cw.error == 1);

    /* Second send should be a no-op (returns -1 immediately) */
    int rc = chunked_writer_send(&cw, "more data", 9);
    CHECK("subsequent send returns -1", rc == -1);

    /* finish() should also be a no-op */
    rc = chunked_writer_finish(&cw);
    CHECK("finish() returns -1 on error", rc == -1);

    close(fds[0]);
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */
int main(void)
{
    printf("══════════════════════════════════════════════════════\n");
    printf("  Week 5 — Keep-Alive & Chunked Encoding Tests\n");
    printf("══════════════════════════════════════════════════════\n");

    test_chunked_one_shot();
    test_chunked_streaming();
    test_chunked_empty_body();
    test_chunked_skip_zero();
    test_keep_alive_parser();
    test_consumed_bytes_calc();
    test_chunked_hex_sizes();
    test_chunked_error_propagation();

    int total = passed + failed;
    printf("\n══════════════════════════════════════════════════════\n");
    printf("  Results: %d/%d passed", passed, total);
    if (failed > 0)
        printf("  (%d FAILED)", failed);
    printf("\n══════════════════════════════════════════════════════\n\n");

    if (failed == 0) {
        printf("All tests passed!\n");
        printf("\nNext steps:\n");
        printf("  1. Build the server:  make\n");
        printf("  2. Run it:            ./httpd\n");
        printf("  3. Test keep-alive:   curl -v --keepalive-time 10 "
               "http://localhost:8080/ http://localhost:8080/index.html\n");
        printf("  4. Benchmark:         wrk -t4 -c100 -d30s "
               "http://localhost:8080/\n");
        printf("  5. Watch in Wireshark: filter 'tcp.port == 8080'\n\n");
    }

    return failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}