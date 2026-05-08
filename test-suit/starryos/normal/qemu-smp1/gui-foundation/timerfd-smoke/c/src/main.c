#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef TFD_NONBLOCK
#define TFD_NONBLOCK O_NONBLOCK
#endif

static int timerfd_create_raw(int clockid, int flags) {
    return (int)syscall(SYS_timerfd_create, clockid, flags);
}

static int timerfd_settime_raw(int fd, int flags, const struct itimerspec *new_value,
                               struct itimerspec *old_value) {
    return (int)syscall(SYS_timerfd_settime, fd, flags, new_value, old_value);
}

static int timerfd_gettime_raw(int fd, struct itimerspec *curr_value) {
    return (int)syscall(SYS_timerfd_gettime, fd, curr_value);
}

static int arm_timer(int fd, long first_ms, long interval_ms) {
    struct itimerspec spec;
    memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_sec = first_ms / 1000;
    spec.it_value.tv_nsec = (first_ms % 1000) * 1000 * 1000;
    spec.it_interval.tv_sec = interval_ms / 1000;
    spec.it_interval.tv_nsec = (interval_ms % 1000) * 1000 * 1000;
    if (timerfd_settime_raw(fd, 0, &spec, NULL) != 0) {
        fprintf(stderr, "FAIL: timerfd_settime: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int read_expirations(int fd, uint64_t *value) {
    ssize_t n = read(fd, value, sizeof(*value));
    if (n != (ssize_t)sizeof(*value)) {
        fprintf(stderr, "FAIL: timerfd read ret=%zd errno=%s\n", n, strerror(errno));
        return 1;
    }
    if (*value == 0) {
        fprintf(stderr, "FAIL: timerfd read returned zero expirations\n");
        return 1;
    }
    return 0;
}

static void sleep_ms(long milliseconds) {
    struct timespec req;
    req.tv_sec = milliseconds / 1000;
    req.tv_nsec = (milliseconds % 1000) * 1000 * 1000;
    nanosleep(&req, NULL);
}

static int test_poll_read(void) {
    int fd = timerfd_create_raw(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "FAIL: timerfd_create nonblock: %s\n", strerror(errno));
        return 1;
    }

    uint64_t value = 0;
    ssize_t n = read(fd, &value, sizeof(value));
    if (n != -1 || errno != EAGAIN) {
        fprintf(stderr, "FAIL: nonblocking disarmed read ret=%zd errno=%s\n", n, strerror(errno));
        close(fd);
        return 1;
    }

    if (arm_timer(fd, 20, 0) != 0) {
        close(fd);
        return 1;
    }

    struct itimerspec current;
    if (timerfd_gettime_raw(fd, &current) != 0) {
        fprintf(stderr, "FAIL: timerfd_gettime armed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    if (current.it_value.tv_sec == 0 && current.it_value.tv_nsec == 0) {
        fprintf(stderr, "FAIL: timerfd_gettime returned disarmed timer\n");
        close(fd);
        return 1;
    }

    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int ready = poll(&pfd, 1, 1000);
    if (ready != 1 || (pfd.revents & POLLIN) == 0) {
        fprintf(stderr, "FAIL: poll timerfd ready=%d revents=%#x errno=%s\n", ready, pfd.revents,
                strerror(errno));
        close(fd);
        return 1;
    }
    if (read_expirations(fd, &value) != 0) {
        close(fd);
        return 1;
    }

    n = read(fd, &value, sizeof(value));
    if (n != -1 || errno != EAGAIN) {
        fprintf(stderr, "FAIL: nonblocking drained read ret=%zd errno=%s\n", n, strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int test_epoll_periodic(void) {
    int fd = timerfd_create_raw(CLOCK_MONOTONIC, 0);
    if (fd < 0) {
        fprintf(stderr, "FAIL: timerfd_create blocking: %s\n", strerror(errno));
        return 1;
    }
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        fprintf(stderr, "FAIL: epoll_create1: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0x5446};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        fprintf(stderr, "FAIL: epoll_ctl add timerfd: %s\n", strerror(errno));
        close(epfd);
        close(fd);
        return 1;
    }

    if (arm_timer(fd, 10, 10) != 0) {
        close(epfd);
        close(fd);
        return 1;
    }

    struct epoll_event out;
    int ready = epoll_wait(epfd, &out, 1, 1000);
    if (ready != 1 || (out.events & EPOLLIN) == 0 || out.data.u64 != 0x5446) {
        fprintf(stderr, "FAIL: epoll_wait timerfd ready=%d events=%#x data=%#llx errno=%s\n",
                ready, out.events, (unsigned long long)out.data.u64, strerror(errno));
        close(epfd);
        close(fd);
        return 1;
    }

    sleep_ms(30);
    uint64_t expirations = 0;
    if (read_expirations(fd, &expirations) != 0) {
        close(epfd);
        close(fd);
        return 1;
    }
    if (expirations < 1) {
        fprintf(stderr, "FAIL: periodic timerfd expiration count=%llu\n",
                (unsigned long long)expirations);
        close(epfd);
        close(fd);
        return 1;
    }

    struct itimerspec disarm;
    struct itimerspec old;
    memset(&disarm, 0, sizeof(disarm));
    memset(&old, 0, sizeof(old));
    if (timerfd_settime_raw(fd, 0, &disarm, &old) != 0) {
        fprintf(stderr, "FAIL: timerfd disarm: %s\n", strerror(errno));
        close(epfd);
        close(fd);
        return 1;
    }
    if (old.it_interval.tv_sec == 0 && old.it_interval.tv_nsec == 0) {
        fprintf(stderr, "FAIL: timerfd old interval was not reported\n");
        close(epfd);
        close(fd);
        return 1;
    }
    if (timerfd_gettime_raw(fd, &old) != 0) {
        fprintf(stderr, "FAIL: timerfd_gettime disarmed: %s\n", strerror(errno));
        close(epfd);
        close(fd);
        return 1;
    }
    if (old.it_value.tv_sec != 0 || old.it_value.tv_nsec != 0) {
        fprintf(stderr, "FAIL: timerfd_gettime disarmed value=%ld.%09ld\n",
                (long)old.it_value.tv_sec, old.it_value.tv_nsec);
        close(epfd);
        close(fd);
        return 1;
    }

    close(epfd);
    close(fd);
    return 0;
}

int main(void) {
    if (test_poll_read() != 0) {
        return 1;
    }
    if (test_epoll_periodic() != 0) {
        return 1;
    }

    printf("timerfd read, poll, epoll, gettime and disarm tests passed\n");
    printf("All timerfd smoke tests passed!\n");
    return 0;
}
