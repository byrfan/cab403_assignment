#define _GNU_SOURCE
#include "car.h"
#include "conn.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define WATCHDOG_NS 500000000LL     /* heartbeat freshness and watchdog: 500 ms */
#define POLL_CAP_NS 100000000LL     /* wake at least this often to notice SIGINT */
#define QN          64              /* queued STATUS snapshots */

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

/* ---------- time helpers ---------- */

static void add_ns(struct timespec *t, long long ns)
{
    t->tv_sec  += (time_t)(ns / 1000000000LL);
    t->tv_nsec += (long)(ns % 1000000000LL);
    if (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
}

static bool reached(const struct timespec *now, const struct timespec *d)
{
    return now->tv_sec > d->tv_sec ||
           (now->tv_sec == d->tv_sec && now->tv_nsec >= d->tv_nsec);
}

static void copy_str(char *dst, const char *src, size_t cap)
{
    strncpy(dst, src, cap - 1);          /* zero-pads, so equal strings compare equal */
    dst[cap - 1] = '\0';
}

/* ---------- shared memory ---------- */

/* Builds "/cab403_car_{name}", writes it to shm_name */
static void build_shm_name(char *shm_name, size_t len, const char *name) {
    snprintf(shm_name, len, "/cab403_car_%s", name);
}

/* Creates and initialises the car's shared memory object.
 * Returns the mapped struct, or NULL on failure (nothing left behind). */
static car_shared_mem *create_shared_mem(const char *name, const char *lowest_floor) {
    char shm_name[64];
    build_shm_name(shm_name, sizeof(shm_name), name);

    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd < 0) {
        fprintf(stderr, "shm_open %s: %s\n", shm_name, strerror(errno));
        return NULL;
    }

    if (ftruncate(fd, sizeof(car_shared_mem)) < 0) {
        fprintf(stderr, "ftruncate %s: %s\n", shm_name, strerror(errno));
        close(fd);
        shm_unlink(shm_name);
        return NULL;
    }

    car_shared_mem *shm = mmap(NULL, sizeof(car_shared_mem),
                               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) {
        fprintf(stderr, "mmap %s: %s\n", shm_name, strerror(errno));
        shm_unlink(shm_name);
        return NULL;
    }

    memset(shm, 0, sizeof *shm);

    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shm->mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&shm->cond, &cattr);
    pthread_condattr_destroy(&cattr);

    copy_str(shm->current_floor, lowest_floor, sizeof shm->current_floor);
    copy_str(shm->target_floor,  lowest_floor, sizeof shm->target_floor);
    strcpy(shm->status, "Closed");
    shm->floor_request[0] = '\0';
    shm->safety_heartbeat = 0;

    return shm;
}

static void destroy_shared_mem(const char *name, car_shared_mem *shm) {
    char shm_name[64];
    build_shm_name(shm_name, sizeof shm_name, name);
    munmap(shm, sizeof *shm);
    shm_unlink(shm_name);
}

/* ---------- floors and argument validation ---------- */

/* Canonical floor: "1".."999" or "B1".."B99", no leading zeros.
 * On success *out is an orderable value (basements negative). Does not modify s. */
static bool floor_sanity_check(const char *floor, int *out)
{
    bool basement = (floor[0] == 'B');
    const char *digits = basement ? floor + 1 : floor;

    size_t len = strlen(digits);
    if (len < 1 || len > (basement ? 2u : 3u)) return false;
    if (digits[0] == '0') return false;

    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)digits[i])) return false;
        n = n * 10 + (digits[i] - '0');
    }

    *out = basement ? -n : n;
    return true;
}

static int floor_value(const char *s)
{
    int n = 0;
    floor_sanity_check(s, &n);
    return n;
}

static bool floor_in_range(const char *f, const car_metadata *md)
{
    int n;
    if (!floor_sanity_check(f, &n)) return false;
    return n >= floor_value(md->low) && n <= floor_value(md->high);
}

/* One floor up (dir=+1) or down (dir=-1), skipping the missing floor 0. */
static bool floor_step(const char *cur, int dir, const car_metadata *md, char *out)
{
    int n = floor_value(cur) + dir;
    if (n == 0) n = dir;
    if (n < floor_value(md->low) || n > floor_value(md->high)) return false;
    char tmp[16];
    if (n < 0) snprintf(tmp, sizeof tmp, "B%d", -n);
    else       snprintf(tmp, sizeof tmp, "%d", n);
    copy_str(out, tmp, 4);
    return true;
}

static bool valid_name(const char *s)
{
    size_t len = strlen(s);
    if (len < 1 || len > 31) return false;
    if (!isalnum((unsigned char)s[0])) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

static bool valid_delay(const char *s, size_t *out)
{
    size_t len = strlen(s);
    if (len < 1 || len > 5 || s[0] == '0') return false;

    unsigned long n = 0;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i])) return false;
        n = n * 10 + (unsigned long)(s[i] - '0');
    }
    if (n < 1 || n > 60000) return false;
    *out = (size_t)n;
    return true;
}

static int map_args(int v, char **c, car_metadata *metadata)
{
    if (v != 5) {
        fprintf(stderr, "Usage: %s {name} {delay} {lowest floor} {highest floor}\n", c[0]);
        return -1;
    }

    int low_n, high_n;

    if (!valid_name(c[1]))                    { fprintf(stderr, "Invalid name\n");  return -1; }
    if (!valid_delay(c[2], &metadata->delay)) { fprintf(stderr, "Invalid delay\n"); return -1; }
    if (!floor_sanity_check(c[3], &low_n) || !floor_sanity_check(c[4], &high_n)) {
        fprintf(stderr, "Invalid floor\n");
        return -1;
    }
    if (low_n >= high_n) {
        fprintf(stderr, "Lowest floor must be below highest floor\n");
        return -1;
    }

    metadata->name = c[1];
    metadata->low  = c[3];
    metadata->high = c[4];
    return 0;
}

/* ---------- network and watchdog thread ---------- */

typedef struct { char cur[4], tgt[4], st[8]; } snap_t;

typedef struct {
    car_shared_mem     *shm;
    const car_metadata *md;
    int       fd;                  /* -1 = disconnected; changed only under the mutex */
    pthread_t rx;
    bool      rx_dead;
    snap_t    sent;                /* last STATUS content sent */
    snap_t    q[QN];               /* STATUS snapshots waiting to be sent, in order */
    int       qh, qn;
    bool      has_pending;         /* FLOOR held back while Between */
    char      pending_floor[4];

    uint32_t  heartbeat_snapshot;  /* last observed heartbeat value */
    bool      hb_seen;             /* a change has been observed at least once */
    bool      hb_in_delay;         /* a change was seen during this reconnect delay */
    struct timespec deadline;      /* watchdog: time of last change + 500 ms */
    struct timespec next_try;      /* earliest time for the next connection attempt */
} net_t;

static void take_snap(const car_shared_mem *s, snap_t *o)
{
    copy_str(o->cur, s->current_floor, sizeof o->cur);
    copy_str(o->tgt, s->target_floor,  sizeof o->tgt);
    copy_str(o->st,  s->status,        sizeof o->st);
}

static bool snap_eq(const snap_t *a, const snap_t *b) { return memcmp(a, b, sizeof *a) == 0; }

/* Mutex held. Queue the current state so every change reaches the controller in
 * order, even if the state moves on again before the net thread wakes. */
static void net_push(net_t *n)
{
    if (n->fd < 0) return;                         /* a new connection sends live state */
    snap_t s;
    take_snap(n->shm, &s);
    const snap_t *last = n->qn ? &n->q[(n->qh + n->qn - 1) % QN] : &n->sent;
    if (snap_eq(&s, last)) return;
    if (n->qn == QN) { n->qh = (n->qh + 1) % QN; n->qn--; }
    n->q[(n->qh + n->qn) % QN] = s;
    n->qn++;
}

static bool send_text(int fd, const char *s)
{
    return send_msg(fd, s, (uint32_t)strlen(s)) == 1;
}

static bool send_status(int fd, const snap_t *s)
{
    char m[48];
    snprintf(m, sizeof m, "STATUS %s %s %s", s->cur, s->tgt, s->st);
    return send_text(fd, m);
}

/* Mutex held on entry and exit, but released inside: re-read state afterwards. */
static void send_unlocked(net_t *n, const char *s)
{
    int fd = n->fd;
    pthread_mutex_unlock(&n->shm->mutex);
    send_text(fd, s);
    pthread_mutex_lock(&n->shm->mutex);
}

/* Same locking rule as send_unlocked. Starts the reconnect delay. */
static void disconnect(net_t *n, long long delay_ms)
{
    if (n->fd < 0) return;
    int fd = n->fd;
    pthread_t rx = n->rx;
    shutdown(fd, SHUT_RDWR);                       /* wakes a blocked recv */
    pthread_mutex_unlock(&n->shm->mutex);
    pthread_join(rx, NULL);
    close(fd);
    pthread_mutex_lock(&n->shm->mutex);

    n->fd = -1;
    n->has_pending = false;                        /* the old controller's route is gone */
    n->hb_in_delay = false;
    n->qh = n->qn = 0;
    clock_gettime(CLOCK_MONOTONIC, &n->next_try);
    add_ns(&n->next_try, delay_ms * 1000000LL);
}

/* Mutex held. Same floor while Closed = door cycle. */
static void set_target(net_t *n, const char *f)
{
    car_shared_mem *s = n->shm;
    copy_str(s->target_floor, f, sizeof s->target_floor);
    if (strcmp(f, s->current_floor) == 0 && strcmp(s->status, "Closed") == 0)
        copy_str(s->status, "Opening", sizeof s->status);
    net_push(n);
}

/* Mutex held, called right after each one-floor movement completes. */
static void net_apply_pending(net_t *n)
{
    if (!n->has_pending) return;
    set_target(n, n->pending_floor);
    n->has_pending = false;
    pthread_cond_broadcast(&n->shm->cond);
}

static void apply_floor(net_t *n, const char *f)
{
    if (!floor_in_range(f, n->md)) return;         /* ignore bad FLOOR */

    car_shared_mem *s = n->shm;
    pthread_mutex_lock(&s->mutex);
    if (n->fd >= 0 && !s->service_mode && !s->emergency_mode) {
        if (strcmp(s->status, "Between") == 0) {
            copy_str(n->pending_floor, f, sizeof n->pending_floor);
            n->has_pending = true;                 /* visible after the floor completes */
        } else {
            set_target(n, f);
            n->has_pending = false;
        }
        pthread_cond_broadcast(&s->cond);
    }
    pthread_mutex_unlock(&s->mutex);
}

static void *rx_thread(void *arg)
{
    net_t *n = arg;
    int fd = n->fd;
    char buf[MAX_MSG_LEN + 1];

    for (;;) {
        int len = recv_message(fd, buf, sizeof buf);
        if (len <= 0) break;
        if (strncmp(buf, "FLOOR ", 6) == 0) apply_floor(n, buf + 6);
    }

    pthread_mutex_lock(&n->shm->mutex);
    n->rx_dead = true;
    pthread_cond_broadcast(&n->shm->cond);
    pthread_mutex_unlock(&n->shm->mutex);
    return NULL;
}

static void net_init(net_t *n, car_shared_mem *shm, const car_metadata *md)
{
    memset(n, 0, sizeof *n);
    n->shm = shm;
    n->md = md;
    n->fd = -1;
    n->heartbeat_snapshot = shm->safety_heartbeat;   /* baseline, not a change */
    clock_gettime(CLOCK_MONOTONIC, &n->deadline);
    clock_gettime(CLOCK_MONOTONIC, &n->next_try);
}

static void *net_thread(void *arg)
{
    net_t *n = arg;
    car_shared_mem *shm = n->shm;
    const long long delay_ms = (long long)n->md->delay;

    pthread_mutex_lock(&shm->mutex);
    while (running) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        /* Heartbeat: re-anchor the deadline ONLY when the value changes. */
        if (shm->safety_heartbeat != n->heartbeat_snapshot) {
            n->heartbeat_snapshot = shm->safety_heartbeat;
            n->hb_seen = true;
            n->deadline = now;
            add_ns(&n->deadline, WATCHDOG_NS);
            if (n->fd < 0 && !reached(&now, &n->next_try)) n->hb_in_delay = true;
        }
        bool expired = !n->hb_seen || reached(&now, &n->deadline);
        bool svc = shm->service_mode == 1;
        bool emg = shm->emergency_mode == 1;

        /* 1. Service/emergency: discard unsent passenger request, notify, disconnect */
        if (svc || emg) {
            if (shm->floor_request_pending) {
                shm->floor_request_pending = 0;
                shm->floor_request[0] = '\0';
                pthread_cond_broadcast(&shm->cond);
            }
            if (n->fd >= 0) {
                send_unlocked(n, emg ? "EMERGENCY" : "SERVICE");
                disconnect(n, 0);
                continue;
            }
        }
        /* 2. Watchdog: connected and the deadline has passed with no change */
        else if (n->fd >= 0 && expired) {
            shm->emergency_mode = 1;
            pthread_cond_broadcast(&shm->cond);
            pthread_mutex_unlock(&shm->mutex);
            fprintf(stderr, "Safety system disconnected! Entering emergency mode.\n");
            fflush(stderr);
            pthread_mutex_lock(&shm->mutex);
            continue;                       /* step 1 now sends EMERGENCY and disconnects */
        }

        /* 3. Passenger request: invalid -> clear; valid -> send if connected, else keep */
        if (!svc && !emg && shm->floor_request_pending == 1) {
            char req[4];
            memcpy(req, shm->floor_request, sizeof req);
            req[3] = '\0';
            if (!floor_in_range(req, n->md)) {
                shm->floor_request_pending = 0;
                shm->floor_request[0] = '\0';
                pthread_cond_broadcast(&shm->cond);
                continue;
            }
            if (n->fd >= 0) {
                char m[16];
                snprintf(m, sizeof m, "SELECT %s", req);
                int fd = n->fd;
                pthread_mutex_unlock(&shm->mutex);
                bool ok = send_text(fd, m);
                pthread_mutex_lock(&shm->mutex);
                if (!ok) { disconnect(n, delay_ms); continue; }
                /* clear only after a successful send, and only if unchanged meanwhile */
                if (shm->floor_request_pending == 1 &&
                    strncmp(shm->floor_request, req, sizeof req - 1) == 0) {
                    shm->floor_request_pending = 0;
                    shm->floor_request[0] = '\0';
                    pthread_cond_broadcast(&shm->cond);
                }
                continue;
            }
        }

        /* 4. Connected: send queued STATUS snapshots in order, then any live change */
        if (n->fd >= 0) {
            if (n->rx_dead) { disconnect(n, delay_ms); continue; }

            snap_t out;
            bool have = false;
            if (n->qn > 0) {
                out = n->q[n->qh];
                n->qh = (n->qh + 1) % QN;
                n->qn--;
                have = true;
            } else {
                take_snap(shm, &out);
                have = !snap_eq(&out, &n->sent);
            }
            if (have) {
                if (snap_eq(&out, &n->sent)) continue;     /* duplicate: skip */
                int fd = n->fd;
                pthread_mutex_unlock(&shm->mutex);
                bool ok = send_status(fd, &out);
                pthread_mutex_lock(&shm->mutex);
                if (!ok) { disconnect(n, delay_ms); continue; }
                n->sent = out;
                continue;                   /* state may have moved on: recheck */
            }
        }
        /* 5. Disconnected: first connection / reconnect.
         * Modes clear, delay over, and a heartbeat change within the last 500 ms
         * (not expired) or during the reconnect delay. */
        else if (!svc && !emg && reached(&now, &n->next_try) &&
                 ((n->hb_seen && !expired) || n->hb_in_delay)) {
            pthread_mutex_unlock(&shm->mutex);
            int fd = init_client();
            if (fd >= 0) {
                struct timeval tv = { 0, 100000 };       /* 100 ms send timeout */
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            }
            pthread_mutex_lock(&shm->mutex);

            if (fd < 0) {                                /* wait at least delay */
                clock_gettime(CLOCK_MONOTONIC, &n->next_try);
                add_ns(&n->next_try, delay_ms * 1000000LL);
                n->hb_in_delay = false;
                continue;
            }

            n->fd = fd;
            n->rx_dead = false;
            n->hb_in_delay = false;
            if (pthread_create(&n->rx, NULL, rx_thread, n) != 0) {
                close(fd);
                n->fd = -1;
                clock_gettime(CLOCK_MONOTONIC, &n->next_try);
                add_ns(&n->next_try, delay_ms * 1000000LL);
                continue;
            }

            /* CAR first, STATUS straight after. Later changes queue behind `sent`. */
            char hello[96];
            snprintf(hello, sizeof hello, "CAR %s %s %s",
                     n->md->name, n->md->low, n->md->high);
            snap_t cur;
            take_snap(shm, &cur);
            n->sent = cur;
            n->qh = n->qn = 0;
            pthread_mutex_unlock(&shm->mutex);
            bool ok = send_text(fd, hello) && send_status(fd, &cur);
            pthread_mutex_lock(&shm->mutex);
            if (!ok) disconnect(n, delay_ms);
            continue;
        }

        /* 6. Sleep until the earliest deadline or any broadcast. A wakeup never moves
         * a deadline, so unrelated signals cannot restart the watchdog. */
        struct timespec wake = now;
        add_ns(&wake, POLL_CAP_NS);
        if (n->fd >= 0 && reached(&wake, &n->deadline))
            wake = n->deadline;
        else if (n->fd < 0 && !reached(&now, &n->next_try) && reached(&wake, &n->next_try))
            wake = n->next_try;
        pthread_cond_timedwait(&shm->cond, &shm->mutex, &wake);
    }

    disconnect(n, 0);                       /* clean shutdown */
    pthread_mutex_unlock(&shm->mutex);
    return NULL;
}

/* ---------- door and movement state machine ---------- */

typedef struct {
    car_shared_mem     *shm;
    const car_metadata *md;
    net_t              *net;
    char     seen[8];          /* status as this loop last saw/set it */
    struct timespec deadline;  /* end of the current timed phase */
    int      move_dir;         /* +1 / -1 for the one-floor movement in progress */
    bool     arrived;          /* a movement just ended at the target: open doors */
    bool     prev_svc;         /* previous service_mode, for edge detection */
    bool     prev_emg;
    bool     changed;          /* we modified shared state: broadcast */
} sm_t;

static bool str_is(const char *a, const char *b) { return strcmp(a, b) == 0; }

static void deadline_from(sm_t *m, const struct timespec *now)
{
    m->deadline = *now;
    add_ns(&m->deadline, (long long)m->md->delay * 1000000LL);
}

static void set_status(sm_t *m, const char *st, const struct timespec *now)
{
    copy_str(m->shm->status, st, sizeof m->shm->status);
    copy_str(m->seen, st, sizeof m->seen);
    deadline_from(m, now);                 /* every phase lasts one delay */
    m->changed = true;
    net_push(m->net);
}

static void reset_target(sm_t *m)
{
    car_shared_mem *s = m->shm;
    if (!str_is(s->target_floor, s->current_floor)) {
        copy_str(s->target_floor, s->current_floor, sizeof s->target_floor);
        m->changed = true;
        net_push(m->net);
    }
}

/* Closed doors, valid in-range target different from current: start one floor. */
static bool start_move(sm_t *m, const struct timespec *now)
{
    car_shared_mem *s = m->shm;
    int cur, tgt;
    if (!floor_sanity_check(s->current_floor, &cur)) return false;
    if (!floor_in_range(s->target_floor, m->md))     return false;
    floor_sanity_check(s->target_floor, &tgt);
    if (tgt == cur) return false;
    m->move_dir = (tgt > cur) ? 1 : -1;
    set_status(m, "Between", now);
    return true;
}

static void finish_move(sm_t *m, const struct timespec *now)
{
    car_shared_mem *s = m->shm;
    char next[4];
    if (floor_step(s->current_floor, m->move_dir, m->md, next))
        copy_str(s->current_floor, next, sizeof s->current_floor);
    set_status(m, "Closed", now);

    if (s->service_mode || s->emergency_mode) {   /* stop here, drop all requests */
        reset_target(m);
        m->net->has_pending = false;
        m->arrived = false;
    } else {
        net_apply_pending(m->net);                /* pending FLOOR becomes visible now */
        m->arrived = str_is(s->status, "Closed") &&
                     str_is(s->target_floor, s->current_floor);
    }
}

static void sm_init(sm_t *m, car_shared_mem *shm, const car_metadata *md, net_t *net)
{
    memset(m, 0, sizeof *m);
    m->shm = shm; m->md = md; m->net = net;
    copy_str(m->seen, shm->status, sizeof m->seen);
}

/* Mutex held on entry and exit; it is released only inside the timed wait. */
static void sm_run(sm_t *m)
{
    car_shared_mem *s = m->shm;

    while (running) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        /* Status changed by someone else (safety: Closing -> Opening, or the net
         * thread's door-cycle request): that phase gets a full delay. */
        if (!str_is(s->status, m->seen)) {
            copy_str(m->seen, s->status, sizeof m->seen);
            deadline_from(m, &now);
        }

        bool svc = (s->service_mode == 1);
        bool emg = (s->emergency_mode == 1);

        /* Entering service mode: drop the route and any held FLOOR. A one-floor
         * movement in progress finishes first; finish_move then resets the target. */
        if (svc && !m->prev_svc) {
            m->net->has_pending = false;
            if (!str_is(s->status, "Between")) reset_target(m);
        }
        m->prev_svc = svc;

        /* Entering emergency mode: discard any held FLOOR. */
        if (emg && !m->prev_emg) m->net->has_pending = false;
        m->prev_emg = emg;

        /* Buttons: always reset to 0 */
        if (s->open_button) {
            s->open_button = 0;
            m->changed = true;
            if (str_is(s->status, "Open"))
                deadline_from(m, &now);                       /* restart open timer */
            else if (str_is(s->status, "Closing") || str_is(s->status, "Closed"))
                set_status(m, "Opening", &now);
        }
        if (s->close_button) {
            s->close_button = 0;
            m->changed = true;
            if (str_is(s->status, "Open")) set_status(m, "Closing", &now);
        }

        /* Timed phase expired: act on the CURRENT status, not the one we expected */
        if (!str_is(s->status, "Closed") && reached(&now, &m->deadline)) {
            if (str_is(s->status, "Between"))       finish_move(m, &now);
            else if (str_is(s->status, "Opening"))  set_status(m, "Open", &now);
            else if (str_is(s->status, "Open")) {
                if (svc) deadline_from(m, &now);              /* no auto-close in service */
                else     set_status(m, "Closing", &now);
            }
            else if (str_is(s->status, "Closing"))  set_status(m, "Closed", &now);
        }

        /* Doors closed and idle */
        if (str_is(s->status, "Closed")) {
            if (m->arrived) {
                m->arrived = false;
                if (!svc && !emg) set_status(m, "Opening", &now);
            } else if (svc) {
                if (!floor_in_range(s->target_floor, m->md)) reset_target(m);
                else start_move(m, &now);                      /* technician: one floor */
            } else if (!emg) {
                start_move(m, &now);                           /* emergency: no new movement */
            }
        }

        if (m->changed) {
            m->changed = false;
            pthread_cond_broadcast(&s->cond);                  /* net thread sends STATUS */
            continue;                                          /* re-evaluate immediately */
        }

        /* Sleep until the phase deadline, or any broadcast, capped for SIGINT */
        struct timespec wake = now;
        add_ns(&wake, POLL_CAP_NS);
        if (!str_is(s->status, "Closed") && reached(&wake, &m->deadline))
            wake = m->deadline;
        pthread_cond_timedwait(&s->cond, &s->mutex, &wake);
    }
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    car_metadata metadata;
    if (map_args(argc, argv, &metadata) != 0) return 1;

    car_shared_mem *shm = create_shared_mem(metadata.name, metadata.low);
    if (!shm) return 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;              /* no SA_RESTART */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    static net_t net;
    net_init(&net, shm, &metadata);
    pthread_t nt;
    if (pthread_create(&nt, NULL, net_thread, &net) != 0) {
        destroy_shared_mem(metadata.name, shm);
        return 1;
    }

    static sm_t sm;
    sm_init(&sm, shm, &metadata, &net);

    pthread_mutex_lock(&shm->mutex);
    sm_run(&sm);
    pthread_cond_broadcast(&shm->cond);     /* wake the net thread */
    pthread_mutex_unlock(&shm->mutex);

    pthread_join(nt, NULL);
    destroy_shared_mem(metadata.name, shm);
    return 0;
}
