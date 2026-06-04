/*
 * server.h — shared constants, includes, and declarations
 */

#ifndef SERVER_H
#define SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <time.h>
#include <limits.h>

#include "http_response.h"

#define SERVER_PORT      8080
#define BACKLOG          128
#define RECV_BUFFER_SIZE 8192

int  create_server_socket(int port);
void handle_client(int client_fd);
void setup_signals(void);
void signal_handler(int sig);

#endif 