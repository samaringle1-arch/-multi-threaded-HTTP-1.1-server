/*
 * config.h — server configuration (Week 5)
 */

#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    int  port;           /* default: 8080            */
    int  threads;        /* default: THREAD_POOL_SIZE */
    char doc_root[512];  /* default: "./www"          */
    int  timeout_sec;    /* default: 30               */
} ServerConfig;

void config_parse(ServerConfig *cfg, int argc, char **argv);
void config_print(const ServerConfig *cfg);

#endif 