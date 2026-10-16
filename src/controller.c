#include "conn.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

static void *client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[MAX_MSG_LEN + 1];

    while (running) {
        int n = recv_message(fd, buf, sizeof buf);
        if (n <= 0) break; 
           
        
    }

    close_client(fd);
    return NULL;
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;                   /* no SA_RESTART: accept() sees EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int lfd = init_server();                     /* loopback:3001, SO_REUSEADDR */
    if (lfd < 0) return 1;

    while (running) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EMFILE || errno == ENFILE) usleep(100000);
            continue;
        }

        pthread_t t;
        if (pthread_create(&t, NULL, client_thread, (void *)(intptr_t)cfd) != 0) {
            close(cfd);                          /* drop this client, keep serving */
            continue;
        }
        pthread_detach(t);
    }

    close(lfd);
    return 0;
}
