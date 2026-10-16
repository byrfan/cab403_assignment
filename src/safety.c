#include "car.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define HEARTBEAT_NS 100000000L        /* 100 ms */

#define REPORT_STOP     0x1u
#define REPORT_OVERLOAD 0x2u
#define REPORT_DATA     0x4u

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

static void add_ns(struct timespec *t, long ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
}

static bool reached(const struct timespec *now, const struct timespec *d)
{
    return now->tv_sec > d->tv_sec ||
           (now->tv_sec == d->tv_sec && now->tv_nsec >= d->tv_nsec);
}

/* ---- validation ---- */

static bool terminated(const char *s, size_t cap) { return memchr(s, '\0', cap) != NULL; }

static bool floor_valid(const char *s, size_t cap)
{
    if (!terminated(s, cap)) return false;
    bool b = (s[0] == 'B');
    const char *d = b ? s + 1 : s;
    size_t len = strlen(d);
    if (len < 1 || len > (b ? 2u : 3u) || d[0] == '0') return false;
    for (size_t i = 0; i < len; i++)
        if (d[i] < '0' || d[i] > '9') return false;
    return true;
}

static bool status_is(const char *s, const char *want)
{
    return terminated(s, 8) && strcmp(s, want) == 0;
}

static bool status_valid(const char *s)
{
    return status_is(s, "Opening") || status_is(s, "Open") || status_is(s, "Closing") ||
           status_is(s, "Closed")  || status_is(s, "Between");
}

static bool data_inconsistent(const car_shared_mem *c)
{
    if (!floor_valid(c->current_floor, sizeof c->current_floor)) return true;
    if (!floor_valid(c->target_floor,  sizeof c->target_floor))  return true;
    if (c->floor_request_pending == 1 &&
        !floor_valid(c->floor_request, sizeof c->floor_request)) return true;
    if (!status_valid(c->status)) return true;

    if (c->open_button > 1 || c->close_button > 1 || c->floor_request_pending > 1 ||
        c->door_obstruction > 1 || c->overload > 1 || c->emergency_stop > 1 ||
        c->service_mode > 1 || c->emergency_mode > 1) return true;

    if (c->door_obstruction == 1 &&
        !status_is(c->status, "Opening") && !status_is(c->status, "Closing")) return true;
    return false;
}

/* Runs the four checks in order, with the mutex held.
 * Returns a bitmask of messages to print after unlocking; sets *changed if state was modified. */
static unsigned run_checks(car_shared_mem *c, bool *changed)
{
    unsigned rep = 0;

    if (c->door_obstruction == 1 && status_is(c->status, "Closing")) {
        memcpy(c->status, "Opening", 8);              /* 7 chars + NUL, fits status[8] */
        *changed = true;
    }
    if (c->emergency_stop == 1 && c->emergency_mode == 0) {
        rep |= REPORT_STOP;
        c->emergency_mode = 1;
        c->emergency_stop = 0;
        *changed = true;
    }
    if (c->overload == 1 && c->emergency_mode == 0) {
        rep |= REPORT_OVERLOAD;
        c->emergency_mode = 1;
        *changed = true;
    }
    if (c->emergency_mode == 0 && data_inconsistent(c)) {
        rep |= REPORT_DATA;
        c->emergency_mode = 1;
        *changed = true;
    }
    return rep;
}

static void print_reports(unsigned rep)
{
    if (rep & REPORT_STOP)
        fprintf(stderr, "The emergency button has been pressed! Entering emergency mode.\n");
    if (rep & REPORT_OVERLOAD)
        fprintf(stderr, "Elevator car is overloaded! Entering emergency mode.\n");
    if (rep & REPORT_DATA)
        fprintf(stderr, "Data consistency error!\n");
    if (rep) fflush(stderr);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s {car name}\n", argv[0]);
        return 1;
    }

    char shm_name[64];
    int n = snprintf(shm_name, sizeof shm_name, "/cab403_car_%s", argv[1]);
    if (n < 0 || (size_t)n >= sizeof shm_name) {
        fprintf(stderr, "Car name too long\n");
        return 1;
    }

    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) {
        fprintf(stderr, "Unable to access car %s.\n", argv[1]);
        return 1;
    }

    car_shared_mem *c = mmap(NULL, sizeof *c, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (c == MAP_FAILED) {
        fprintf(stderr, "mmap failed\n");
        return 1;
    }
    
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    add_ns(&deadline, HEARTBEAT_NS);

    pthread_mutex_lock(&c->mutex);
    while (running) {
        bool changed = false;
        unsigned rep = run_checks(c, &changed);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (reached(&now, &deadline)) {
            c->safety_heartbeat++;                    /* unsigned wraparound is intended */
            changed = true;
            add_ns(&deadline, HEARTBEAT_NS);          /* fixed period: never reset by wakeups */
        }

        if (changed) pthread_cond_broadcast(&c->cond);

        if (rep) {                                    /* print outside the lock */
            pthread_mutex_unlock(&c->mutex);
            print_reports(rep);
            pthread_mutex_lock(&c->mutex);
            continue;                                 /* state may have changed while unlocked */
        }

        /* Wakes on any broadcast (run the checks again) or at the heartbeat deadline */
        pthread_cond_timedwait(&c->cond, &c->mutex, &deadline);
    }
    pthread_mutex_unlock(&c->mutex);

    munmap(c, sizeof *c);
    return 0;
}
