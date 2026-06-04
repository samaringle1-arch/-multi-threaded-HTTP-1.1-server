# httpd — Multi-threaded HTTP/1.1 Server in C

A production-grade static file server built from scratch using only POSIX APIs.
No external libraries. No frameworks. Every layer hand-written and explained.

```
$ ./server --port 8080 --root ./www --threads 4
[config] port        : 8080
[config] doc_root    : ./www
[config] threads     : 4
[config] timeout_sec : 30
[pool]   started 4 worker threads  (queue capacity: 128)
[server] listening on port 8080  (pid 12345)
```

---

## Benchmark

Tested on Ubuntu 22.04, Intel Core i5, 4 cores, localhost.

| Mode | Req/sec | p50 | p90 | p99 | Failed |
|------|--------:|----:|----:|----:|-------:|
| Keep-alive (`wrk -t4 -c100 -d60s`) | **16,275** | 213μs | 345μs | 8ms | 0 |
| Stress (`ab -n 50000 -c 50`) | **14,800** | — | — | — | 0 |

```
wrk -t4 -c100 -d60s --latency http://localhost:8080/index.html

Running 60s test @ http://localhost:8080/index.html
  4 threads and 100 connections
  Latency Distribution
     50%  213.00us
     90%  345.00us
     99%    8.12ms
  Requests/sec: 16275.49
  Transfer/sec:    27.72MB
```

---

## How it works

The server is built in six independent layers. Each layer is a separate
`.c` file with its own header and its own test suite. A bug in one layer
cannot corrupt another — they communicate only through well-defined structs.

```
┌─────────────────────────────────────────────────────┐
│                     server.c                         │
│         main() · accept loop · signals               │
├──────────────┬──────────────────────────────────────┤
│ thread_pool  │        keep_alive                    │
│  4 workers   │   per-connection request loop        │
│  mutex+condv │   SO_RCVTIMEO · pipelining           │
├──────────────┴──────────────────────────────────────┤
│              http_parser                             │
│   state machine · partial read handling             │
│   method · path · query · headers · body            │
├─────────────────────────────────────────────────────┤
│   file_server          │     http_response           │
│   realpath() guard     │   status line · headers    │
│   MIME table           │   sendfile(2) · chunked    │
│   dir listing          │   error pages              │
├────────────────────────┴────────────────────────────┤
│              Linux kernel / POSIX                    │
│   TCP sockets · pthreads · sendfile · SO_RCVTIMEO   │
└─────────────────────────────────────────────────────┘
```

### Layer 1 — TCP socket server (`server.c`)

The entry point. Creates a TCP socket, binds to a port, and runs an
`accept()` loop. Each accepted file descriptor is handed to the thread
pool immediately — the main thread never blocks in I/O.

Key decisions:
- `SO_REUSEADDR` — allows restart within the TCP TIME_WAIT window
- `TCP_NODELAY` — disables Nagle's algorithm, reduces latency for
  request/response protocols
- `SIGPIPE` ignored — prevents crash when client disconnects mid-write
- `SIGINT`/`SIGTERM` sets a flag that breaks the accept loop cleanly

### Layer 2 — Thread pool (`thread_pool.c`)

A fixed-size pool of worker threads sharing a circular-buffer work queue.

```
main thread                    workers (×4)
───────────                    ─────────────────
accept(fd)          →  queue   dequeue(fd)
thread_pool_submit  →  ─────   handle_connection(fd)
loop                   mutex   close(fd)
                       condv   wait for next item
```

Synchronisation uses two condition variables:
- `not_empty` — workers block here when the queue is empty
- `not_full`  — main thread blocks here when the queue is full (backpressure)

Shutdown is graceful: setting `shutdown=1` and broadcasting both condvars
causes workers to drain remaining items before exiting. `pthread_join()`
ensures every in-flight request completes before the process exits.

### Layer 3 — HTTP/1.1 parser (`http_parser.c`)

A pure function — takes a `char *` buffer and length, returns a typed
`HttpRequest` struct. Knows nothing about sockets or threads.

```c
ParseResult parse_http_request(const char *raw, size_t len,
                               HttpRequest *req);
// Returns: PARSE_OK · PARSE_INCOMPLETE · PARSE_ERROR
```

The parser handles partial reads correctly. TCP does not preserve message
boundaries — a single HTTP request can arrive in 3 separate `recv()`
calls. The caller feeds the growing buffer on every `recv()` and the
parser returns `PARSE_INCOMPLETE` until `\r\n\r\n` has arrived.

The off-by-one fix: `header_end` points to the `\r` of `\r\n\r\n`. The
last header's `\n` sits at `header_end + 1`. The search window must be
`header_end + 2` to include it — a subtle bug that the 40-test suite
caught on day one.

### Layer 4 — Keep-alive connection loop (`keep_alive.c`)

Replaces the single-request `handle_client()` from earlier weeks with a
loop that serves multiple requests per connection.

```
handle_connection(fd):
  set SO_RCVTIMEO = 30 seconds
  loop:
    accumulate bytes → parse_http_request()
    dispatch_request() → serve_static()
    if Connection: close → break
    if requests >= 1000 → break
    memmove leftover bytes to front of buffer   ← pipelining support
  close(fd)
```

The sliding window (`memmove`) is the key correctness detail. When a
client pipelines two requests in one TCP segment, after parsing request 1
the remaining bytes are the start of request 2. Moving them to the front
of the buffer lets the next iteration parse correctly without data loss.

### Layer 5 — Static file server (`file_server.c`)

Serves files from a configurable document root with three security
properties:

**Path traversal protection** — every URI path is resolved through
`realpath()` which canonicalises all `..` segments and symlinks. The
resolved path must begin with `doc_root` or the request gets a 403.

```
GET /../../../etc/passwd
→ realpath("./www/../../etc/passwd") = "/etc/passwd"
→ "/etc/passwd" does not start with "./www" → 403 Forbidden
```

**Zero-copy file transfer** — files are sent with `sendfile(2)`, which
transfers from the kernel's page cache directly to the socket buffer.
The file data never enters userspace. For a 1 MB file, this eliminates
two memory copies compared to `read()` + `write()`.

**MIME type detection** — 24-entry table keyed on file extension.
Case-insensitive (`photo.JPG` and `photo.jpg` both return `image/jpeg`).
Unknown extensions fall back to `application/octet-stream`.

### Layer 6 — Response builder & chunked encoding (`http_response.c`, `chunked.c`)

`HttpResponse` struct holds status code and headers. Headers are
serialised into a single buffer and sent in one `write()` call to avoid
Nagle's algorithm splitting them across packets.

Chunked transfer encoding follows RFC 7230 §4.1 wire format:

```
<hex-length>\r\n
<chunk-data>\r\n
...
0\r\n
\r\n
```

Used for responses of unknown length (directory listings, error pages)
where `Content-Length` cannot be set before sending starts.

---

## Project structure

```
.
├── server.c              Main — accept loop, signals, config wiring
├── server.h              Shared includes and constants
├── http_parser.c/h       HTTP/1.1 request parser (pure function)
├── http_response.c/h     Response builder, sendfile wrapper
├── file_server.c/h       Static file serving, MIME types, path guard
├── thread_pool.c/h       Fixed thread pool, work queue
├── keep_alive.c/h        Persistent connection loop, pipelining
├── chunked.c/h           Chunked Transfer-Encoding encoder
├── config.c/h            CLI argument parsing, defaults
├── fuzz_parser.c         AFL-ready stdin fuzzer for the HTTP parser
│
├── test_parser.c         40 parser unit tests
├── test_thread_pool.c    14 concurrency + drain tests
├── test_file_server.c    35 MIME, path sanitisation, header tests
├── test_keep_alive.c     8 keep-alive + chunked encoding tests
│
├── www/                  Document root (served files)
│   ├── index.html
│   ├── about.html
│   ├── data.json
│   ├── css/style.css
│   ├── js/app.js
│   └── subdir/
│
└── Makefile
```

---

## Build & run

**Requirements:** GCC, Make, Linux (or WSL2 on Windows).

```bash
# Build
make

# Run with defaults (port 8080, ./www, 4 threads, 30s timeout)
make run

# Custom configuration
./server --port 9090 --root /var/www --threads 8 --timeout 10

# Help
./server --help
```

## Test

```bash
# Run all 97 unit tests across all modules
make test

# Individual suites
make test_parser    # HTTP parser
make test_pool      # Thread pool concurrency
make test_files     # File server + MIME + path sanitisation
make test_ka        # Keep-alive + chunked encoding

# Race condition check (requires valgrind)
make helgrind
```

## Benchmark

```bash
# Install tools
sudo apt install apache2-utils
git clone https://github.com/wg/wrk && cd wrk && make && sudo cp wrk /usr/local/bin

# Throughput + latency percentiles
wrk -t4 -c100 -d60s --latency http://localhost:8080/index.html

# Zero-failure stress test
ab -n 50000 -c 50 http://localhost:8080/index.html | grep -E "Requests per|Failed|Time per"
```

---

## Design decisions

**Why a fixed thread pool instead of one thread per connection?**
One-thread-per-connection hits OS limits fast — Linux defaults to 1024
threads per process. A pool of 4 threads handles thousands of concurrent
keep-alive connections because most are idle between requests. The pool
size matches CPU core count; I/O wait means adding more threads beyond
that gives diminishing returns.

**Why `sendfile()` instead of `read()` + `write()`?**
`sendfile()` is a Linux syscall that transfers from a file's page cache
directly to a socket's send buffer in kernel space. The file data never
touches userspace memory. For static file serving (our entire workload),
this eliminates 2 memory copies per request and reduces CPU usage
measurably under load.

**Why `SO_RCVTIMEO` for keep-alive timeout instead of a timer thread?**
`SO_RCVTIMEO` is set once per socket and makes `recv()` return
`EAGAIN` after the timeout fires. No additional threads, no `select()`
loop, no complexity. It's the right tool for a blocking-I/O model.
An `epoll`-based server would use a timeout wheel instead.

**Why `realpath()` for path sanitisation instead of string scanning?**
String-scanning approaches (`strstr(path, "..")`) miss encoding tricks
like `%2e%2e` or null bytes. `realpath()` resolves the canonical
absolute path through the kernel, handling all edge cases correctly.
The check is simple: if the result doesn't start with `doc_root`, refuse.

---

## What I learned building this

- TCP does not preserve message boundaries. Every recv() loop must handle
  partial reads — this is not optional or an edge case.
- The C memory model matters. Holding a mutex across a blocking recv()
  call would serialise all 4 workers back to single-threaded. The lock
  must be released before any I/O.
- Unit tests saved hours. The off-by-one in the header parser (the
  `header_end + 2` fix) was caught immediately by the test suite. Without
  tests it would have silently dropped the last header on every request.
- Benchmarking numbers only mean something when Failed requests = 0.
  A server with 50,000 req/s and 1,000 failures is a broken server.

---

## Future improvements

### Short term (1–2 weeks each)

**epoll event loop** — Replace the blocking thread pool with a
single-threaded `epoll` event loop. Eliminates thread context switching
overhead and scales to 100k+ concurrent connections on a single core.
Expected throughput improvement: 2–3×.

**TLS/HTTPS with OpenSSL** — Wrap the socket layer with `SSL_read()`/
`SSL_write()`. Add certificate loading via CLI flag. Enables HTTPS on
`localhost` for testing and makes the project deployable.

**HTTP range requests** — Implement `Range: bytes=0-1023` header support.
Required for video streaming and resumable downloads. Adds `206 Partial
Content` responses and `Content-Range` header.

**`If-Modified-Since` caching** — Check request's `If-Modified-Since`
header against file's `mtime`. If unchanged, return `304 Not Modified`
with no body. Eliminates redundant data transfer for unchanged static
files — the single biggest real-world performance win.

**Gzip compression** — Use `zlib` to compress responses above a size
threshold when client sends `Accept-Encoding: gzip`. Reduces transfer
size by 60–80% for HTML/CSS/JS. Adds `Content-Encoding: gzip` header.

### Medium term (1–2 months)

**HTTP/2 support** — Binary framing protocol with header compression
(HPACK) and multiplexing. A complete rewrite of the framing layer but
re-uses the routing and file serving code. Requires TLS (HTTP/2 over
cleartext is rarely supported by browsers).

**Virtual hosting** — Inspect the `Host` header and serve from a
different document root per hostname. Allows hosting multiple sites on
one server instance. Adds a `VirtualHost` config directive.

**Reverse proxy mode** — Forward requests to upstream servers based on
path prefix. Adds load balancing logic and upstream health checks. Turns
the server into a lightweight nginx alternative for development.

**CGI/FastCGI** — Fork a process or connect to a FastCGI socket to run
dynamic scripts. Enables PHP, Python, or any executable to generate
responses. Adds the CGI environment variable protocol.

**Access logging** — Write one line per request to a log file in Combined
Log Format (`127.0.0.1 - - [date] "GET /path" 200 1234`). Add log
rotation on SIGHUP. Makes the server observable in production.

### Long term (research / advanced)

**io_uring** — Replace `sendfile()` and `recv()` with Linux's new
asynchronous I/O interface. Eliminates system call overhead by batching
I/O operations in a shared ring buffer between userspace and kernel.
The highest-performance path available on modern Linux.

**QUIC / HTTP/3** — UDP-based transport with built-in encryption and
multiplexing without head-of-line blocking. Requires implementing or
integrating a QUIC library (e.g., ngtcp2). The direction the web is
heading.

**Zero-copy receive** — Use `MSG_ZEROCOPY` with `SO_ZEROCOPY` to avoid
copying received data from kernel to userspace. Combined with `sendfile()`
for transmit, achieves true zero-copy for proxy/pass-through workloads.

---

## References

- Beej's Guide to Network Programming — beej.us/guide/bgnet
- RFC 7230 — HTTP/1.1 Message Syntax and Routing
- RFC 7231 — HTTP/1.1 Semantics and Content
- The Linux Programming Interface (Kerrisk) — chapters 29–33, 56–61
- POSIX Threads Programming — computing.llnl.gov/tutorials/pthreads

---

## Author

**Samar** — built over 5 weeks as a deep-dive into systems programming,
network protocols, and concurrent C.
