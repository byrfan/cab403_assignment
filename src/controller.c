#define _GNU_SOURCE
#include "conn.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CARS    32
#define MAX_CONNS   256
#define MAX_SWEEPS  16
#define MAX_STOPS   1100          /* B99..999 is 1098 floors */
#define NONE        INT_MIN

/* Floors are stored as contiguous "ordinals": B2 = -1, B1 = 0, 1 = 1, 2 = 2 */

static bool floor_to_ord(const char *s, int *ord)
{
    int v;
    if (!util_floor(s, &v)) return false;
    *ord = (v < 0) ? v + 1 : v;
    return true;
}

static void ord_to_floor(int o, char out[16])
{
    if (o <= 0) snprintf(out, 16, "B%d", 1 - o);
    else        snprintf(out, 16, "%d", o);
}

/* A route is a list of sweeps. Each sweep has a direction and floors sorted
 * monotonically in that direction, every stop in a sweep is serviced in that
 * sweep's direction, so (floor, direction) is implied by (sweep, floor). */
typedef struct {
    int dir;                      /* +1 up, -1 down */
    int n;
    int floors[MAX_STOPS];
} sweep_t;

typedef struct {
    bool     used;
    uint64_t id;                  /* tells a replaced car from its replacement */
    int      fd;
    char     name[32];
    int      lo, hi;
    bool     sched;               /* valid CAR and valid STATUS received */
    int      cur, tgt;
    char     status[8];
    int      commanded;           /* floor last sent in FLOOR, or NONE */
    int      nsw;
    sweep_t  sw[MAX_SWEEPS];
} car_t;

static pthread_mutex_t g = PTHREAD_MUTEX_INITIALIZER;   /* guards everything below */
static car_t     cars[MAX_CARS];
static uint64_t  next_id = 1;
static int       conn_fds[MAX_CONNS];                   /* open sockets, 0 = free */

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

/* One nonblocking write of the whole frame: a slow car can never stall the
 * controller, and a peer that can't take a tiny frame is dropped. */
static bool send_frame(int fd, const char *msg)
{
    uint8_t buf[4 + 128];
    size_t len = strlen(msg);
    if (len > 128) return false;
    uint32_t nl = htonl((uint32_t)len);
    memcpy(buf, &nl, 4);
    memcpy(buf + 4, msg, len);

    ssize_t r;
    do {
        r = send(fd, buf, 4 + len, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (r < 0 && errno == EINTR);
    return r == (ssize_t)(4 + len);
}

/* Caller holds g. Only shuts the socket down; the owning thread closes it. */
static void remove_car(car_t *c)
{
    if (!c->used) return;
    shutdown(c->fd, SHUT_RDWR);
    c->used = false;
    c->nsw = 0;
}

static int sign(int x) { return (x > 0) - (x < 0); }

/* If Closing or Between, the effective position is the next floor in the
 * direction of movement; otherwise it is the current floor. */
static int effective_pos(const car_t *c)
{
    if (strcmp(c->status, "Closing") != 0 && strcmp(c->status, "Between") != 0)
        return c->cur;

    int dir = sign(c->tgt - c->cur);
    if (dir == 0 && c->nsw) dir = sign(c->sw[0].floors[0] - c->cur);
    if (dir == 0 && c->nsw) dir = c->sw[0].dir;
    if (dir == 0) return c->cur;

    int p = c->cur + dir;
    if (p < c->lo) p = c->lo;
    if (p > c->hi) p = c->hi;
    return p;
}

/* Sorted insert in the sweep's direction; duplicates coalesce. */
static bool sweep_add(sweep_t *s, int f)
{
    int i = 0;
    while (i < s->n && s->dir * s->floors[i] < s->dir * f) i++;
    if (i < s->n && s->floors[i] == f) return true;
    if (s->n >= MAX_STOPS) return false;
    memmove(&s->floors[i + 1], &s->floors[i], (size_t)(s->n - i) * sizeof(int));
    s->floors[i] = f;
    s->n++;
    return true;
}

/* x = +1 UP hall call, -1 DOWN hall call, 0 passenger SELECT.
 * Sweep 0 is in progress: only floors still ahead of the car can join it.
 * Later sweeps accept any floor. */
static bool route_insert(car_t *c, int f, int x)
{
    int pos = effective_pos(c);

    for (int k = 0; k < c->nsw; k++) {
        sweep_t *s = &c->sw[k];
        if (x != 0 && s->dir != x) continue;                   /* wrong-direction sweep */
        if (k == 0 && s->dir * (f - pos) < 0) continue;        /* behind the car */
        return sweep_add(s, f);
    }

    if (c->nsw >= MAX_SWEEPS) return false;
    int d;
    if (x != 0)       d = x;
    else if (c->nsw)  d = -c->sw[c->nsw - 1].dir;              /* behind: next opposite sweep */
    else              d = (f >= pos) ? 1 : -1;

    sweep_t *s = &c->sw[c->nsw++];
    s->dir = d;
    s->n = 0;
    return sweep_add(s, f);
}

static void pop_first(car_t *c)
{
    sweep_t *s = &c->sw[0];
    memmove(&s->floors[0], &s->floors[1], (size_t)(s->n - 1) * sizeof(int));
    if (--s->n == 0) {
        memmove(&c->sw[0], &c->sw[1], (size_t)(c->nsw - 1) * sizeof(sweep_t));
        c->nsw--;
    }
}

/* Send FLOOR only when the first queued stop differs from what was already
 * commanded, so an in-flight acknowledgement never causes a resend. */
static void dispatch(car_t *c)
{
    if (!c->used || !c->sched) return;
    if (c->nsw == 0) { c->commanded = NONE; return; }

    int first = c->sw[0].floors[0];
    if (first == c->commanded) return;

    char fl[16], msg[32];
    ord_to_floor(first, fl);
    snprintf(msg, sizeof msg, "FLOOR %s", fl);
    if (!send_frame(c->fd, msg)) { remove_car(c); return; }
    c->commanded = first;
}

/* ---- message handlers (all called with g held) ---- */

static car_t *register_car(int fd, const char *name, int lo, int hi)
{
    for (int i = 0; i < MAX_CARS; i++)                  /* same name replaces the old car */
        if (cars[i].used && strcmp(cars[i].name, name) == 0) remove_car(&cars[i]);

    for (int i = 0; i < MAX_CARS; i++) {
        car_t *c = &cars[i];
        if (c->used) continue;
        c->used = true;
        c->id = next_id++;
        c->fd = fd;
        snprintf(c->name, sizeof c->name, "%s", name);
        c->lo = lo; c->hi = hi;
        c->sched = false;
        c->cur = c->tgt = lo;
        strcpy(c->status, "Closed");
        c->commanded = NONE;
        c->nsw = 0;
        return c;
    }
    return NULL;
}

static bool on_status(car_t *c, const char *a, const char *b, const char *st)
{
    static const char *ok_st[] = { "Opening", "Open", "Closing", "Closed", "Between" };
    int cur, tgt;
    if (!floor_to_ord(a, &cur) || !floor_to_ord(b, &tgt)) return false;
    if (cur < c->lo || cur > c->hi || tgt < c->lo || tgt > c->hi) return false;

    bool known = false;
    for (size_t i = 0; i < sizeof ok_st / sizeof *ok_st; i++)
        if (strcmp(st, ok_st[i]) == 0) known = true;
    if (!known) return false;

    bool was_opening = c->sched && strcmp(c->status, "Opening") == 0 && c->cur == cur;

    c->cur = cur;
    c->tgt = tgt;
    snprintf(c->status, sizeof c->status, "%s", st);
    c->sched = true;

    /* Transition into Opening at the first queued floor: that stop is done. */
    if (!was_opening && strcmp(st, "Opening") == 0 &&
        c->nsw && c->sw[0].floors[0] == cur) {
        pop_first(c);
        c->commanded = NONE;
    }
    dispatch(c);
    return c->used;
}

static bool on_select(car_t *c, const char *fl)
{
    int f;
    if (!c->sched || !floor_to_ord(fl, &f) || f < c->lo || f > c->hi) return false;
    if (!route_insert(c, f, 0)) return false;
    dispatch(c);
    return c->used;
}

/* CALL {car name} {floor} {UP|DOWN}  ->  ACCEPTED | UNAVAILABLE */
static bool on_call(int fd, const char *name, const char *fl, const char *dir)
{
    int f, x;
    if (!util_valid_name(name) || !floor_to_ord(fl, &f)) return false;   /* malformed: drop */
    if (strcmp(dir, "UP") == 0)        x = 1;
    else if (strcmp(dir, "DOWN") == 0) x = -1;
    else return false;

    car_t *c = NULL;
    for (int i = 0; i < MAX_CARS; i++)
        if (cars[i].used && cars[i].sched && strcmp(cars[i].name, name) == 0) {
            c = &cars[i];
            break;
        }

    /* UNAVAILABLE: car missing/not ready, floor outside its range, or the
     * direction is impossible from the end of its range. Opposite-direction
     * travel is NOT a reason: that call waits for a later sweep. */
    bool ok = c && f >= c->lo && f <= c->hi &&
              !(x == 1 && f == c->hi) && !(x == -1 && f == c->lo) &&
              route_insert(c, f, x);
    if (ok) {
        dispatch(c);
        ok = c->used;            /* dispatch drops the car if its send failed */
    }
    return send_frame(fd, ok ? "ACCEPTED" : "UNAVAILABLE");
}

static int split(char *s, char *tok[], int max)
{
    int n = 0;
    char *p;
    while ((p = strsep(&s, " ")) != NULL) {
        if (n == max) return max + 1;            /* too many tokens */
        tok[n++] = p;
    }
    return n;
}

/* Returns false to invalidate (drop) the connection. */
static bool handle_msg(int fd, car_t **pc, uint64_t *pid, char *msg)
{
    char *tok[5];
    int n = split(msg, tok, 4);
    bool ok = false;

    pthread_mutex_lock(&g);
    car_t *c = *pc;
    if (c && (!c->used || c->id != *pid)) {      /* replaced or removed elsewhere */
        pthread_mutex_unlock(&g);
        return false;
    }

    if (strcmp(tok[0], "CAR") == 0) {
        int lo, hi;
        if (!c && n == 4 && util_valid_name(tok[1]) &&
            floor_to_ord(tok[2], &lo) && floor_to_ord(tok[3], &hi) && lo < hi) {
            c = register_car(fd, tok[1], lo, hi);
            if (c) { *pc = c; *pid = c->id; ok = true; }
        }
    } else if (strcmp(tok[0], "STATUS") == 0) {
        ok = c && n == 4 && on_status(c, tok[1], tok[2], tok[3]);
    } else if (strcmp(tok[0], "SELECT") == 0) {
        ok = c && n == 2 && on_select(c, tok[1]);
    } else if (strcmp(tok[0], "SERVICE") == 0 || strcmp(tok[0], "EMERGENCY") == 0) {
        if (c && n == 1) remove_car(c);          /* car and its whole route are gone */
        ok = false;
    } else if (strcmp(tok[0], "CALL") == 0) {
        ok = !c && n == 4 && on_call(fd, tok[1], tok[2], tok[3]);
    }

    if (c && !c->used) ok = false;
    pthread_mutex_unlock(&g);
    return ok;
}

/* ---- connections ---- */

static bool track_fd(int fd)
{
    for (int i = 0; i < MAX_CONNS; i++)
        if (conn_fds[i] == 0) { conn_fds[i] = fd; return true; }
    return false;
}

/* True if fd was still tracked (so the caller must close it). */
static bool untrack_fd(int fd)
{
    for (int i = 0; i < MAX_CONNS; i++)
        if (conn_fds[i] == fd) { conn_fds[i] = 0; return true; }
    return false;
}

/* One thread per connection: a slow or idle peer only blocks its own thread. */
static void *conn_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[MAX_MSG_LEN + 1];
    car_t *car = NULL;
    uint64_t id = 0;

    while (running) {
        int n = recv_message(fd, buf, sizeof buf);
        if (n <= 0) break;
        if (!handle_msg(fd, &car, &id, buf)) break;
    }

    pthread_mutex_lock(&g);
    if (car && car->used && car->id == id) remove_car(car);   /* disconnect: drop car + route */
    bool mine = untrack_fd(fd);
    pthread_mutex_unlock(&g);
    if (mine) close(fd);
    return NULL;
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;                 /* no SA_RESTART so accept() sees EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int lfd = init_server();
    if (lfd < 0) return 1;

    while (running) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EMFILE || errno == ENFILE) usleep(100000);
            continue;
        }

        pthread_mutex_lock(&g);
        bool tracked = track_fd(cfd);
        pthread_mutex_unlock(&g);
        if (!tracked) { close(cfd); continue; }

        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)cfd) != 0) {
            pthread_mutex_lock(&g);
            bool mine = untrack_fd(cfd);
            pthread_mutex_unlock(&g);
            if (mine) close(cfd);
            continue;
        }
        pthread_detach(t);
    }

    /* SIGINT: stop accepting, close every active socket, exit */
    close(lfd);
    pthread_mutex_lock(&g);
    for (int i = 0; i < MAX_CONNS; i++) {
        if (conn_fds[i]) {
            shutdown(conn_fds[i], SHUT_RDWR);
            close(conn_fds[i]);
            conn_fds[i] = 0;
        }
    }
    pthread_mutex_unlock(&g);
    return 0;
}
