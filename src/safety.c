/* 
 * safety.c - safety monitor for one elevator car: ./safety {car name}
 *
 * Build: gcc -std=gnu11 -O2 -pthread -Wall -Wextra -Wpedantic -Wconversion \
 *            -Wsign-conversion -Wshadow -Wcast-qual -o safety safety.c
 *
 * DEVIATIONS (MISRA C:2012 / safety-critical guidance)
 *
 * 
 *
 * D1  Rule 21.5  signal.h used (sigaction, sig_atomic_t, SIGINT).
 *     Needed to stop cleanly on SIGINT (unlock, unmap). The handler only
 *     stores 0 into a volatile sig_atomic_t; all other work is in normal
 *     context.
 * D2  Rule 21.10 time.h used (struct timespec, clock_gettime).
 *     Needed for the monotonic 100 ms heartbeat and the absolute timed wait.
 *     None of the banned functions (time, clock, ...) are used.
 * D3  Rule 11.5  void * from mmap() converted to car_shared_mem *.
 *     mmap returns void * by definition. The object size is checked with
 *     fstat() first, so the mapping is at least sizeof(car_shared_mem).
 * D4  Rules 11.4/11.6  comparison with MAP_FAILED, which POSIX defines as
 *     ((void *)-1). It is the only specified way to detect mmap failure and
 *     is only compared, never dereferenced.
 * D5  Rule 10.1  PROT_READ | PROT_WRITE are int macros made to be OR'd
 *     together. Both are constants, so the result is fixed at compile time.
 * D6  Dir 4.6  plain int/long/char/ssize_t/off_t/time_t are used where POSIX
 *     or the fixed car.h layout dictates them. uint32_t is used elsewhere.
 * D7  Rule 1.2  built with -std=gnu11 only so POSIX declarations are
 *     visible. No GNU language extensions are used.
 * D8  Termination/blocking. The monitor loop runs until SIGINT because the
 *     heartbeat must be continuous. Each pass is bounded: the only wait is
 *     pthread_cond_timedwait() with an absolute deadline, so the program
 *     never blocks forever, and no other loop is unbounded.
 * D9  POSIX calls (shm_open, mmap, pthread_*, write ...) are outside the
 *     MISRA standard library rules and are required by the interface.
 * D10 car.h is supplied and must not be changed. The unused typedef/tag/
 *     macro findings in it (Rules 2.3, 2.4, 2.5) are inherited, not ours.
 *
 * Other notes
 *  - No stdio. Diagnostics use write(2) on stderr, which is unbuffered, so
 *    every message is out as soon as write returns (no flush needed).
 *  - No dynamic memory, recursion, goto, break, continue or VLAs.
 *  - One return per function; every POSIX return value is checked. The only
 *    (void) casts are on memcpy/memset, close() after mapping, and stderr
 *    writes (nothing useful to do if stderr itself fails).
 *  - Strings in shared memory are never trusted: every read is bounded by
 *    the field size and checked for a terminator.
 *  - The mutex is not held while printing.
 *  - status[] must hold "Opening" + NUL (8 bytes), as car.h defines it.
 */

// for the record i abhor misra
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "car.h"

#define HEARTBEAT_NS   100000000L    /* 100 ms */
#define NS_PER_SEC     1000000000L
#define NAME_MAX_LEN   31U
#define NAME_CAP       64U
#define MSG_CAP        128U

#define REPORT_STOP     0x1U
#define REPORT_OVERLOAD 0x2U
#define REPORT_DATA     0x4U

static volatile sig_atomic_t running = 1;

static void on_sigint(int sig)
{
    (void)sig;
    running = 0;
}

/* ---- helpers ---- */

static size_t blen(const char *s, size_t cap)
{
    size_t n = 0U;
    while ((n < cap) && (s[n] != '\0')) {
        n++;
    }
    return n;
}

static void say(const char *s)
{
    const ssize_t w = write(STDERR_FILENO, s, blen(s, MSG_CAP));
    (void)w;
}

static void add_ns(struct timespec *t, long ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= NS_PER_SEC) {
        t->tv_nsec -= NS_PER_SEC;
        t->tv_sec += 1;
    }
}

static bool reached(const struct timespec *now, const struct timespec *d)
{
    return (now->tv_sec > d->tv_sec) ||
           ((now->tv_sec == d->tv_sec) && (now->tv_nsec >= d->tv_nsec));
}

/* ---- validation of shared data ---- */

/* bounded string equality; an unterminated field is never equal */
static bool str_is(const char *f, size_t cap, const char *lit)
{
    size_t i = 0U;
    while ((i < cap) && (f[i] == lit[i]) && (f[i] != '\0')) {
        i++;
    }
    return (i < cap) && (f[i] == '\0') && (lit[i] == '\0');
}

/* "1".."999" or "B1".."B99", no leading zero */
static bool floor_valid(const char *s, size_t cap)
{
    const size_t len    = blen(s, cap);
    const size_t start  = (s[0] == 'B') ? 1U : 0U;
    const size_t maxdig = (start == 1U) ? 2U : 3U;
    bool ok = (len < cap) && (len > start) && ((len - start) <= maxdig) &&
              (s[start] != '0');
    size_t i;

    for (i = start; ok && (i < len); i++) {
        if ((s[i] < '0') || (s[i] > '9')) {
            ok = false;
        }
    }
    return ok;
}

static bool status_is(const car_shared_mem *c, const char *want)
{
    return str_is(c->status, sizeof(c->status), want);
}

static bool data_inconsistent(const car_shared_mem *c)
{
    const bool floors_bad =
        !floor_valid(c->current_floor, sizeof(c->current_floor)) ||
        !floor_valid(c->target_floor, sizeof(c->target_floor));

    const bool request_bad = (c->floor_request_pending == 1U) &&
        !floor_valid(c->floor_request, sizeof(c->floor_request));

    const bool opening = status_is(c, "Opening");
    const bool closing = status_is(c, "Closing");
    const bool status_bad = !(opening || closing || status_is(c, "Open") ||
                              status_is(c, "Closed") || status_is(c, "Between"));

    const bool flags_bad =
        (c->open_button > 1U) || (c->close_button > 1U) ||
        (c->floor_request_pending > 1U) || (c->door_obstruction > 1U) ||
        (c->overload > 1U) || (c->emergency_stop > 1U) ||
        (c->service_mode > 1U) || (c->emergency_mode > 1U);

    const bool obstruction_bad = (c->door_obstruction == 1U) && !opening && !closing;

    return floors_bad || request_bad || status_bad || flags_bad || obstruction_bad;
}

/* The four checks in order, mutex held. Returns messages to print later. */
static uint32_t run_checks(car_shared_mem *c, bool *changed)
{
    uint32_t rep = 0U;

    if ((c->door_obstruction == 1U) && status_is(c, "Closing")) {
        (void)memcpy(c->status, "Opening", sizeof("Opening"));
        *changed = true;
    }
    if ((c->emergency_stop == 1U) && (c->emergency_mode == 0U)) {
        rep |= REPORT_STOP;
        c->emergency_mode = 1U;
        c->emergency_stop = 0U;
        *changed = true;
    }
    if ((c->overload == 1U) && (c->emergency_mode == 0U)) {
        rep |= REPORT_OVERLOAD;
        c->emergency_mode = 1U;
        *changed = true;
    }
    if ((c->emergency_mode == 0U) && data_inconsistent(c)) {
        rep |= REPORT_DATA;
        c->emergency_mode = 1U;
        *changed = true;
    }
    return rep;
}

static void print_reports(uint32_t rep)
{
    if ((rep & REPORT_STOP) != 0U) {
        say("The emergency button has been pressed! Entering emergency mode.\n");
    }
    if ((rep & REPORT_OVERLOAD) != 0U) {
        say("Elevator car is overloaded! Entering emergency mode.\n");
    }
    if ((rep & REPORT_DATA) != 0U) {
        say("Data consistency error!\n");
    }
}

/* ---- shared memory ---- */

static bool build_name(char *dst, const char *name)
{
    static const char prefix[] = "/cab403_car_";
    const size_t plen = sizeof(prefix) - 1U;
    const size_t n = blen(name, NAME_CAP);
    bool ok = (n >= 1U) && (n <= NAME_MAX_LEN);
    size_t i;

    if (ok) {
        for (i = 0U; i < plen; i++) {
            dst[i] = prefix[i];
        }
        for (i = 0U; ok && (i < n); i++) {
            if (name[i] == '/') {
                ok = false;
            } else {
                dst[plen + i] = name[i];
            }
        }
        dst[plen + n] = '\0';
    }
    return ok;
}

/* Opens and maps the car's object; NULL on any failure. */
static car_shared_mem *map_car(const char *shm_name)
{
    car_shared_mem *result = NULL;
    const int fd = shm_open(shm_name, O_RDWR, 0);

    if (fd >= 0) {
        struct stat info;
        (void)memset(&info, 0, sizeof(info));

        if ((fstat(fd, &info) == 0) && (info.st_size >= 0) &&
            ((uint64_t)info.st_size >= (uint64_t)sizeof(car_shared_mem))) {
            void *const map = mmap(NULL, sizeof(car_shared_mem),
                                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (map != MAP_FAILED) {
                result = (car_shared_mem *)map;
            }
        }
        (void)close(fd);    /* mapping stays valid after close */
    }
    return result;
}

/* ---- monitor ---- */

/* One pass. Returns false on any error; *locked says if we still hold the mutex. */
static bool cycle(car_shared_mem *c, struct timespec *dl, bool *locked)
{
    bool changed = false;
    struct timespec now = { 0, 0 };
    const uint32_t rep = run_checks(c, &changed);
    bool ok = (clock_gettime(CLOCK_MONOTONIC, &now) == 0);

    if (ok && reached(&now, dl)) {
        c->safety_heartbeat = c->safety_heartbeat + 1U;   /* wraparound intended */
        changed = true;
        add_ns(dl, HEARTBEAT_NS);       /* fixed period, never reset by wakeups */
    }
    if (ok && changed) {
        ok = (pthread_cond_broadcast(&c->cond) == 0);
    }

    if (ok) {
        if (rep != 0U) {
            /* print with the mutex released; next pass re-checks the state */
            ok = (pthread_mutex_unlock(&c->mutex) == 0);
            if (ok) {
                *locked = false;
                print_reports(rep);
                ok = (pthread_mutex_lock(&c->mutex) == 0);
                *locked = ok;
            }
        } else {
            /* wakes on any broadcast or at the heartbeat deadline */
            const int rc = pthread_cond_timedwait(&c->cond, &c->mutex, dl);
            ok = (rc == 0) || (rc == ETIMEDOUT);
        }
    }
    return ok;
}

static int monitor(car_shared_mem *c)
{
    int result = EXIT_FAILURE;
    struct timespec dl = { 0, 0 };

    if (clock_gettime(CLOCK_MONOTONIC, &dl) == 0) {
        add_ns(&dl, HEARTBEAT_NS);

        if (pthread_mutex_lock(&c->mutex) == 0) {
            bool locked = true;
            bool ok = true;

            while ((running != 0) && ok) {
                ok = cycle(c, &dl, &locked);
            }
            if (locked) {
                ok = ok && (pthread_mutex_unlock(&c->mutex) == 0);
            }
            if (ok) {
                result = EXIT_SUCCESS;
            }
        }
    }
    return result;
}

static bool install_handler(void)
{
    struct sigaction sa;

    (void)memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;      /* no SA_RESTART */
    return (sigemptyset(&sa.sa_mask) == 0) && (sigaction(SIGINT, &sa, NULL) == 0);
}

static int run(const char *car_name)
{
    int result = EXIT_FAILURE;
    char shm_name[NAME_CAP] = { 0 };
    car_shared_mem *c = NULL;

    if (build_name(shm_name, car_name)) {
        c = map_car(shm_name);
    }

    if (c == NULL) {
        say("Unable to access car ");
        say(car_name);
        say(".\n");
    } else {
        if (install_handler()) {
            result = monitor(c);
        } else {
            say("Unable to install signal handler.\n");
        }
        if (munmap(c, sizeof(*c)) != 0) {
            result = EXIT_FAILURE;
        }
    }
    return result;
}

int main(int argc, char **argv)
{
    int result = EXIT_FAILURE;

    if (argc != 2) {
        say("Usage: safety {car name}\n");
    } else {
        result = run(argv[1]);
    }
    return result;
}
