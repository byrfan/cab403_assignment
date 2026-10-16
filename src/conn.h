#ifndef CONN_H
#define CONN_H

#include <stdint.h>

#define ADDR         "127.0.0.1"
#define PORT         3001
#define MAX_MSG_LEN  4096      /* longest valid message body */

int init_server(void);                                   /* controller: bind + listen */
int init_client(void);                                   /* car/call: connect to controller */
int close_client(int fd);
int send_msg(int fd, const char *buf, uint32_t len);     /* 1 ok, 0 peer closed, -1 error */
int recv_message(int fd, char *buf, uint32_t bufsize);   /* len > 0, 0 closed, -1 error */

#endif
