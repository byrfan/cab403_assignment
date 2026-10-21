#define _GNU_SOURCE
#include "conn.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ./call {car name} {floor} {up|down} */
int main(int argc, char **argv)
{
    if (argc != 4) { // testing, could be removed but no matter
        fprintf(stderr, "Usage: %s {car name} {floor} {up|down}\n", argv[0]);
        return 1;
    }

    int fv;
    const char *dir;
    if (!util_valid_name(argv[1]))  { fprintf(stderr, "Invalid car name.\n");            return 1; }
    if (!util_floor(argv[2], &fv))  { fprintf(stderr, "Invalid floor specified.\n");     return 1; }
    if (strcmp(argv[3], "up") == 0)        dir = "UP";
    else if (strcmp(argv[3], "down") == 0) dir = "DOWN";
    else { fprintf(stderr, "Invalid direction specified.\n"); return 1; }

    int fd = init_client();
    if (fd < 0) {
        fprintf(stderr, "Unable to connect to elevator system.\n");
        return 1;
    }

    struct timeval tv = { 5, 0 };                      /* never hang forever on a reply */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    char msg[96];
    int n = snprintf(msg, sizeof msg, "CALL %s %s %s", argv[1], argv[2], dir);
    if (send_msg(fd, msg, (uint32_t)n) != 1) {
        fprintf(stderr, "Unable to connect to elevator system.\n");
        close_client(fd);
        return 1;
    }

    char reply[MAX_MSG_LEN + 1];
    int len = recv_message(fd, reply, sizeof reply);
    close_client(fd);

    if (len > 0 && strcmp(reply, "ACCEPTED") == 0) {
        printf("Car %s is arriving.\n", argv[1]);
        return 0;
    }
    if (len > 0 && strcmp(reply, "UNAVAILABLE") == 0) {
        printf("Sorry, that car is unavailable for this request.\n");
        return 0;
    }
    fprintf(stderr, "Unable to connect to elevator system.\n");
    return 1;
}
