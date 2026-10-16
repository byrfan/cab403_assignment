#define _GNU_SOURCE
#include "car.h"
#include "util.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(const char *p)
{
    fprintf(stderr, "Usage: %s {open|close|stop|service_on|service_off|up|down} {car name}\n"
                    "       %s floor {car name} {floor}\n", p, p);
}

/* Maps the car's object, or returns NULL after printing an error. */
static car_shared_mem *open_car(const char *name)
{
    char path[64];
    snprintf(path, sizeof path, "/cab403_car_%s", name);

    int fd = shm_open(path, O_RDWR, 0);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || (size_t)st.st_size < sizeof(car_shared_mem)) {
        if (fd >= 0) close(fd);
        fprintf(stderr, "Unable to access car %s.\n", name);
        return NULL;
    }
    car_shared_mem *c = mmap(NULL, sizeof *c, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (c == MAP_FAILED) {
        fprintf(stderr, "Unable to access car %s.\n", name);
        return NULL;
    }
    return c;
}

static bool status_is(const car_shared_mem *c, const char *s)
{
    return memchr(c->status, '\0', sizeof c->status) && strcmp(c->status, s) == 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *op = argv[1];
    const char *name = argv[2];
    bool is_floor = strcmp(op, "floor") == 0;

    static const char *simple[] = { "open", "close", "stop", "service_on",
                                    "service_off", "up", "down" };
    bool known = is_floor;
    for (size_t i = 0; i < sizeof simple / sizeof *simple; i++)
        if (strcmp(op, simple[i]) == 0) known = true;

    if (!known)                                    { fprintf(stderr, "Invalid operation.\n"); usage(argv[0]); return 1; }
    if (argc != (is_floor ? 4 : 3))                { usage(argv[0]); return 1; }
    if (!util_valid_name(name))                    { fprintf(stderr, "Invalid car name.\n"); return 1; }

    int fval = 0;
    if (is_floor && !util_floor(argv[3], &fval))   { fprintf(stderr, "Invalid floor specified.\n"); return 1; }

    car_shared_mem *c = open_car(name);
    if (!c) return 1;

    int rc = 0;
    pthread_mutex_lock(&c->mutex);

    if (strcmp(op, "open") == 0) {
        c->open_button = 1;
    } else if (strcmp(op, "close") == 0) {
        c->close_button = 1;
    } else if (strcmp(op, "stop") == 0) {
        c->emergency_stop = 1;
    } else if (strcmp(op, "service_on") == 0) {
        c->service_mode = 1;
        c->emergency_mode = 0;
        c->floor_request_pending = 0;
        c->floor_request[0] = '\0';
    } else if (strcmp(op, "service_off") == 0) {
        c->service_mode = 0;
    } else if (is_floor) {
        if (c->service_mode == 1) {
            fprintf(stderr, "Operation not allowed while in service mode.\n"); rc = 1;
        } else if (c->emergency_mode == 1) {
            fprintf(stderr, "Operation not allowed while in emergency mode.\n"); rc = 1;
        } else if (c->floor_request_pending == 1) {
            fprintf(stderr, "A floor request is already pending.\n"); rc = 1;
        } else {
            memset(c->floor_request, 0, sizeof c->floor_request);
            strncpy(c->floor_request, argv[3], sizeof c->floor_request - 1);
            c->floor_request_pending = 1;
        }
    } else {                                           /* up / down */
        int dir = (strcmp(op, "up") == 0) ? 1 : -1;
        int cur;
        if (c->service_mode != 1) {
            fprintf(stderr, "Operation only allowed in service mode.\n"); rc = 1;
        } else if (status_is(c, "Between")) {
            fprintf(stderr, "Operation not allowed while elevator is moving.\n"); rc = 1;
        } else if (!status_is(c, "Closed")) {
            fprintf(stderr, "Operation not allowed while doors are open.\n"); rc = 1;
        } else if (strcmp(c->target_floor, c->current_floor) != 0) {
            fprintf(stderr, "Operation not allowed while elevator is moving.\n"); rc = 1;
        } else if (!util_floor(c->current_floor, &cur)) {
            fprintf(stderr, "Unable to access car %s.\n", name); rc = 1;
        } else {
            int next = cur + dir;
            if (next == 0) next = dir;                 /* B1 <-> 1, there is no floor 0 */
            if (next > 999 || next < -99) {
                fprintf(stderr, "Invalid floor specified.\n"); rc = 1;
            } else {
                if (next < 0) snprintf(c->target_floor, sizeof c->target_floor, "B%d", -next);
                else          snprintf(c->target_floor, sizeof c->target_floor, "%d", next);
            }
        }
    }

    if (rc == 0) pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->mutex);
    munmap(c, sizeof *c);
    return rc;
}
