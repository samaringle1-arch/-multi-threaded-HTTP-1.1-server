// fuzz_parser.c
#include "http_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, stdin);
    buf[n] = '\0';

    HttpRequest req;
    parse_http_request(buf, n, &req);
    // We only care that this never crashes or hangs.
    // Any ParseResult (OK/ERROR/INCOMPLETE) is fine.
    return 0;
}