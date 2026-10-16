#include "car.h"

#include <bits/time.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <ctype.h>   


#define WATCHDOG_NS 500000000LL // heartbeat for watchdog

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

/* Builds "/car{name}", writes it to shm_name */
static void build_shm_name(char *shm_name, size_t len, const char *name) {
    snprintf(shm_name, len, "/cab403_car_%s", name);
}

/*
 * Creates and initialises the car's shared memory object.
 * Returns the mapped struct, or NULL on failure (nothing left behind).
 */
car_shared_mem *create_shared_mem(const char *name, const char *lowest_floor) {
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
    close(fd);                      /* mapping stays valid after close */
    if (shm == MAP_FAILED) {
        fprintf(stderr, "mmap %s: %s\n", shm_name, strerror(errno));
        shm_unlink(shm_name);
        return NULL;
    }

    /* Zero everything first: covers every uint8_t, the heartbeat and strings */
    memset(shm, 0, sizeof *shm);

    /* Process-shared mutex and condition variable */
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

    /* Spec-mandated initial values */
    strncpy(shm->current_floor, lowest_floor, sizeof shm->current_floor - 1);
    strncpy(shm->target_floor,  lowest_floor, sizeof shm->target_floor - 1);
    strcpy(shm->status, "Closed");
    shm->floor_request[0] = '\0';
    shm->safety_heartbeat = 0;

    return shm;
}

/* Call from the SIGINT cleanup path */
void destroy_shared_mem(const char *name, car_shared_mem *shm) {
    char shm_name[64];
    build_shm_name(shm_name, sizeof shm_name, name);
    munmap(shm, sizeof *shm);
    shm_unlink(shm_name);
}

/* Canonical floor check: "1".."999" or "B1".."B99", no leading zeros.
 * On success writes an orderable value to *out (basements negative).
 * Does NOT modify the string. */
bool floor_sanity_check(const char *floor, int *out)
{
    bool basement = (floor[0] == 'B');
    const char *digits = basement ? floor + 1 : floor;

    size_t len = strlen(digits);
    if (len < 1 || len > (basement ? 2u : 3u)) return false;
    if (digits[0] == '0') return false;          /* "0", "B0", "007" */

    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)digits[i])) return false;
        n = n * 10 + (digits[i] - '0');
    }

    *out = basement ? -n : n;
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
    if (len < 1 || len > 5 || s[0] == '0') return false;   /* canonical */

    unsigned long n = 0;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i])) return false;
        n = n * 10 + (unsigned long)(s[i] - '0');
    }
    if (n < 1 || n > 60000) return false;
    *out = (size_t)n;
    return true;
}

/* Returns 0 on success, -1 on invalid arguments */
int map_args(int v, char **c, car_metadata *metadata)
{
    int low_n, high_n;

    if (!valid_name(c[1]))                  { fprintf(stderr, "Invalid name\n");  return -1; }
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

int main(int argc, char **argv)
{
    car_metadata metadata;
    if (map_args(argc, argv, &metadata) != 0) return 1;

    car_shared_mem *shm = create_shared_mem(metadata.name, metadata.low);
    if (!shm) return 1;
    /* 
    pthread_mutex_lock(&shm->mutex);
    shm->emergency_stop = 1;
    pthread_cond_broadcast(&shm->cond);
    pthread_mutex_unlock(&shm->mutex);
    */ // testing
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    uint32_t heartbeat_snapshot = shm->safety_heartbeat;
    add_ns(&deadline, WATCHDOG_NS);
    
    // TODO this will need to be put inside a thread as the watchdog shouldnt stop the car from responding to events
    while(1)
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        if(reached(&now, &deadline) && shm->safety_heartbeat > heartbeat_snapshot) 
        {
           // in here handle first connection 
        } 
        else add_ns(&deadline, WATCHDOG_NS);
    }
    
    while(1) {} // actual main loop here

    destroy_shared_mem(metadata.name, shm);
    return 0;
}
