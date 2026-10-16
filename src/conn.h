#ifndef CONN_H
#define CONN_H

#define PORT 3001
#define ADDR "127.0.0.1"
#define MAX_MSG_LEN 4096

#include <sys/socket.h>
#include <string.h>
#include <arpa/inet.h>

int init_server(void);
int init_client(void);
int close_client(int fd);
int send_msg(int fd, const char *buf, uint32_t len);
int recv_message(int fd, char *buf, uint32_t bufsize);

#endif
