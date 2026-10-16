#include "conn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* Controller: listening socket on loopback:PORT with address reuse. */
int init_server(void)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(sockfd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, ADDR, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return -1;
    }

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, SOMAXCONN) < 0) {
        perror("listen");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/* Car: connects to the controller. Returns a connected fd, or -1.
 * Silent on connect failure: the caller retries after `delay` ms. */
int init_client(void)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, ADDR, &addr.sin_addr) <= 0) {
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/* shutdown() first so a thread blocked in recv() wakes up */
int close_client(int fd)
{
    shutdown(fd, SHUT_RDWR);
    if (close(fd) < 0) return -1;
    return 0;
}

/* Returns n on success, 0 if the peer closed, -1 on error */
static ssize_t write_stream_all(int fd, const void *buf, size_t n)
{
    size_t total = 0;
    while (total < n) {
        ssize_t wrote = send(fd, (const char *)buf + total, n - total, MSG_NOSIGNAL);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (wrote == 0) return 0;
        total += (size_t)wrote;
    }
    return (ssize_t)total;
}

/* Returns 1 on success, 0 if the peer closed, -1 on error */
int send_msg(int fd, const char *buf, uint32_t len)
{
    uint32_t net_len = htonl(len);
    ssize_t r = write_stream_all(fd, &net_len, sizeof(net_len));
    if (r == 0) return 0;
    if (r < 0) return -1;

    r = write_stream_all(fd, buf, len);
    if (r == 0) return 0;
    if (r < 0) return -1;

    return 1;
}

/* Reads exactly n bytes. Returns n, 0 on clean close, -1 on error */
static ssize_t read_stream_all(int fd, void *buf, size_t n)
{
    size_t total = 0;
    while (total < n) {
        ssize_t got = recv(fd, (char *)buf + total, n - total, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (got == 0) return 0;
        total += (size_t)got;
    }
    return (ssize_t)total;
}

/* Returns message length (> 0) with buf NUL-terminated,
 * 0 if the peer closed, -1 on error or a bad length. */
int recv_message(int fd, char *buf, uint32_t bufsize)
{
    uint32_t net_len;
    ssize_t r = read_stream_all(fd, &net_len, sizeof(net_len));
    if (r == 0) return 0;
    if (r < 0) return -1;

    uint32_t len = ntohl(net_len);
    if (len == 0) return -1;
    if (len > MAX_MSG_LEN || len >= bufsize) {      /* >= leaves room for '\0' */
        fprintf(stderr, "rejected message: length %u exceeds limit\n", len);
        return -1;
    }

    r = read_stream_all(fd, buf, len);
    if (r <= 0) return -1;                          /* close mid-message is an error */

    buf[len] = '\0';
    return (int)len;
}
