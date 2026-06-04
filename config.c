/*
 * config.c — CLI argument parsing and defaults (Week 5)
 */

#include "config.h"
#include "thread_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void config_parse(ServerConfig *cfg, int argc, char **argv)
{
    /* Fill defaults first */
    cfg->port        = 8080;
    cfg->threads     = THREAD_POOL_SIZE;
    cfg->timeout_sec = 30;
    strncpy(cfg->doc_root, "./www", sizeof(cfg->doc_root) - 1);

    /* Parse --key value pairs */
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--port") == 0) {
            cfg->port = atoi(argv[++i]);

        } else if (strcmp(argv[i], "--root") == 0) {
            strncpy(cfg->doc_root, argv[++i],
                    sizeof(cfg->doc_root) - 1);

        } else if (strcmp(argv[i], "--threads") == 0) {
            cfg->threads = atoi(argv[++i]);

        } else if (strcmp(argv[i], "--timeout") == 0) {
            cfg->timeout_sec = atoi(argv[++i]);

        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: ./server [options]\n");
            printf("  --port N      listen port       (default: 8080)\n");
            printf("  --root PATH   document root     (default: ./www)\n");
            printf("  --threads N   worker threads    (default: %d)\n",
                   THREAD_POOL_SIZE);
            printf("  --timeout N   keep-alive secs   (default: 30)\n");
            exit(EXIT_SUCCESS);
        }
    }

    /* Clamp to sane ranges */
    if (cfg->port    <  1)   cfg->port    = 8080;
    if (cfg->port    > 65535) cfg->port   = 8080;
    if (cfg->threads <  1)   cfg->threads = 1;
    if (cfg->threads > 64)   cfg->threads = 64;
    if (cfg->timeout_sec < 1) cfg->timeout_sec = 1;
}


void config_print(const ServerConfig *cfg)
{
    printf("[config] port        : %d\n",  cfg->port);
    printf("[config] doc_root    : %s\n",  cfg->doc_root);
    printf("[config] threads     : %d\n",  cfg->threads);
    printf("[config] timeout_sec : %d\n",  cfg->timeout_sec);
}