/*
  server.c — Week 5: keep-alive & chunked encoding
 */

#include "server.h"
#include "keep_alive.h"
#include "http_parser.h"
#include "thread_pool.h"
#include "config.h"

static volatile sig_atomic_t g_running = 1;


void handle_client(int client_fd)
{
    handle_connection(client_fd);
}


int main(int argc, char *argv[])
{
   
    ServerConfig cfg;
    config_parse(&cfg, argc, argv);
    config_print(&cfg);

    setup_signals();

    
    int server_fd = create_server_socket(cfg.port);

    printf("[server] Week 5 — keep-alive + chunked encoding\n");
    printf("[server] listening on port %d  (pid %d)\n",
           cfg.port, getpid());
    printf("[server] document root       : %s\n",  cfg.doc_root);
    printf("[server] keep-alive timeout  : %ds\n", cfg.timeout_sec);
    printf("[server] worker threads      : %d\n",  cfg.threads);
    printf("[server] benchmark: wrk -t4 -c100 -d30s "
           "http://localhost:%d/\n\n", cfg.port);

    ThreadPool pool;
    thread_pool_init(&pool);

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t          client_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) break;
            perror("[server] accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr,
                  client_ip, sizeof(client_ip));
        printf("[server] connection from %s:%d  fd=%d\n",
               client_ip, ntohs(client_addr.sin_port), client_fd);

        thread_pool_submit(&pool, client_fd, client_addr);
    }

    printf("[server] shutting down...\n");
    thread_pool_destroy(&pool);
    close(server_fd);
    return EXIT_SUCCESS;
}



int create_server_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

   
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

#ifdef TCP_NODELAY
    {
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                   &nodelay, sizeof(nodelay));
    }
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); exit(EXIT_FAILURE);
    }
    if (listen(fd, BACKLOG) < 0) {
        perror("listen"); close(fd); exit(EXIT_FAILURE);
    }
    return fd;
}

void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}