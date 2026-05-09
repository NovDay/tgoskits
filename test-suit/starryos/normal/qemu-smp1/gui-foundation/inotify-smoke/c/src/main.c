#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef IN_CLOEXEC
#define IN_CLOEXEC O_CLOEXEC
#endif

#ifndef IN_NONBLOCK
#define IN_NONBLOCK O_NONBLOCK
#endif

static int inotify_init1_raw(int flags) {
    return (int)syscall(SYS_inotify_init1, flags);
}

static int inotify_add_watch_raw(int fd, const char *path, uint32_t mask) {
    return (int)syscall(SYS_inotify_add_watch, fd, path, mask);
}

static int inotify_rm_watch_raw(int fd, int wd) {
    return (int)syscall(SYS_inotify_rm_watch, fd, wd);
}

static int read_event(int fd, struct inotify_event *event, size_t event_size) {
    memset(event, 0, event_size);
    ssize_t n = read(fd, event, event_size);
    if (n < (ssize_t)sizeof(*event)) {
        fprintf(stderr, "FAIL: read inotify event ret=%zd errno=%s\n", n, strerror(errno));
        return 1;
    }
    return 0;
}

static int expect_named_event(int fd, int wd, uint32_t mask, const char *name) {
    char buf[sizeof(struct inotify_event) + 64];
    struct inotify_event *event = (struct inotify_event *)buf;
    if (read_event(fd, event, sizeof(buf)) != 0) {
        return 1;
    }
    if (event->wd != wd || event->mask != mask || event->len == 0 ||
        strcmp(event->name, name) != 0) {
        fprintf(stderr, "FAIL: inotify event wd=%d mask=%#x len=%u name=%s expected wd=%d mask=%#x name=%s\n",
                event->wd, event->mask, event->len, event->len == 0 ? "(none)" : event->name,
                wd, mask, name);
        return 1;
    }
    return 0;
}

static int expect_unnamed_event(int fd, int wd, uint32_t mask) {
    struct inotify_event event;
    if (read_event(fd, &event, sizeof(event)) != 0) {
        return 1;
    }
    if (event.wd != wd || event.mask != mask || event.cookie != 0 || event.len != 0) {
        fprintf(stderr, "FAIL: unnamed inotify event wd=%d mask=%#x cookie=%u len=%u expected wd=%d mask=%#x\n",
                event.wd, event.mask, event.cookie, event.len, wd, mask);
        return 1;
    }
    return 0;
}

static bool is_named_event(const struct inotify_event *event, int wd, uint32_t mask,
                           const char *name) {
    return event->wd == wd && event->mask == mask && event->len != 0 &&
           strcmp(event->name, name) == 0;
}

static bool is_unnamed_event(const struct inotify_event *event, int wd, uint32_t mask) {
    return event->wd == wd && event->mask == mask && event->cookie == 0 && event->len == 0;
}

static int expect_named_and_unnamed_events(int fd, int named_wd, uint32_t named_mask,
                                           const char *name, int unnamed_wd,
                                           uint32_t unnamed_mask) {
    char first_buf[sizeof(struct inotify_event) + 64];
    char second_buf[sizeof(struct inotify_event) + 64];
    struct inotify_event *first = (struct inotify_event *)first_buf;
    struct inotify_event *second = (struct inotify_event *)second_buf;

    if (read_event(fd, first, sizeof(first_buf)) != 0 ||
        read_event(fd, second, sizeof(second_buf)) != 0) {
        return 1;
    }

    bool first_named = is_named_event(first, named_wd, named_mask, name);
    bool second_named = is_named_event(second, named_wd, named_mask, name);
    bool first_unnamed = is_unnamed_event(first, unnamed_wd, unnamed_mask);
    bool second_unnamed = is_unnamed_event(second, unnamed_wd, unnamed_mask);

    if ((first_named && second_unnamed) || (first_unnamed && second_named)) {
        return 0;
    }

    fprintf(stderr,
            "FAIL: paired inotify events first=(wd=%d mask=%#x len=%u name=%s) second=(wd=%d mask=%#x len=%u name=%s) expected named wd=%d mask=%#x name=%s and unnamed wd=%d mask=%#x\n",
            first->wd, first->mask, first->len, first->len == 0 ? "(none)" : first->name,
            second->wd, second->mask, second->len, second->len == 0 ? "(none)" : second->name,
            named_wd, named_mask, name, unnamed_wd, unnamed_mask);
    return 1;
}

int main(void) {
    const char *path = "/tmp";
    const char *dir_path = "/tmp/inotify-smoke-dir";
    const char *file_name = "watched-file";
    const char *file_path = "/tmp/inotify-smoke-dir/watched-file";

    unlink(file_path);
    rmdir(dir_path);
    if (mkdir(dir_path, 0700) != 0) {
        fprintf(stderr, "FAIL: mkdir %s: %s\n", dir_path, strerror(errno));
        return 1;
    }

    int fd = inotify_init1_raw(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "FAIL: inotify_init1: %s\n", strerror(errno));
        return 1;
    }

    char buf[sizeof(struct inotify_event)];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n != -1 || errno != EAGAIN) {
        fprintf(stderr, "FAIL: empty nonblocking inotify read ret=%zd errno=%s\n", n,
                strerror(errno));
        close(fd);
        return 1;
    }

    if (inotify_add_watch_raw(fd, path, IN_ONLYDIR) != -1 || errno != EINVAL) {
        fprintf(stderr, "FAIL: inotify_add_watch flag-only mask errno=%s\n", strerror(errno));
        close(fd);
        return 1;
    }

    int wd = inotify_add_watch_raw(fd, path, IN_CREATE | IN_DELETE | IN_MODIFY | IN_ONLYDIR);
    if (wd < 0) {
        fprintf(stderr, "FAIL: inotify_add_watch %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }

    int wd_again = inotify_add_watch_raw(fd, path, IN_MODIFY | IN_MASK_ADD);
    if (wd_again != wd) {
        fprintf(stderr, "FAIL: inotify_add_watch IN_MASK_ADD wd=%d expected=%d errno=%s\n",
                wd_again, wd, strerror(errno));
        close(fd);
        return 1;
    }

    if (inotify_rm_watch_raw(fd, wd) != 0) {
        fprintf(stderr, "FAIL: inotify_rm_watch: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int ready = poll(&pfd, 1, 1000);
    if (ready != 1 || (pfd.revents & POLLIN) == 0) {
        fprintf(stderr, "FAIL: poll inotify ready=%d revents=%#x errno=%s\n", ready,
                pfd.revents, strerror(errno));
        close(fd);
        return 1;
    }

    struct inotify_event event;
    memset(&event, 0, sizeof(event));
    n = read(fd, &event, sizeof(event));
    if (n != (ssize_t)sizeof(event)) {
        fprintf(stderr, "FAIL: read inotify ignored event ret=%zd errno=%s\n", n,
                strerror(errno));
        close(fd);
        return 1;
    }
    if (event.wd != wd || event.mask != IN_IGNORED || event.cookie != 0 || event.len != 0) {
        fprintf(stderr, "FAIL: inotify ignored event wd=%d mask=%#x cookie=%u len=%u\n",
                event.wd, event.mask, event.cookie, event.len);
        close(fd);
        return 1;
    }

    n = read(fd, buf, sizeof(buf));
    if (n != -1 || errno != EAGAIN) {
        fprintf(stderr, "FAIL: drained nonblocking inotify read ret=%zd errno=%s\n", n,
                strerror(errno));
        close(fd);
        return 1;
    }

    int dir_wd = inotify_add_watch_raw(fd, dir_path,
                                       IN_CREATE | IN_DELETE | IN_MODIFY | IN_OPEN |
                                           IN_CLOSE_WRITE | IN_CLOSE_NOWRITE | IN_ATTRIB |
                                           IN_ONLYDIR);
    if (dir_wd < 0) {
        fprintf(stderr, "FAIL: inotify_add_watch %s: %s\n", dir_path, strerror(errno));
        close(fd);
        rmdir(dir_path);
        return 1;
    }

    int file_fd = open(file_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (file_fd < 0) {
        fprintf(stderr, "FAIL: open create %s: %s\n", file_path, strerror(errno));
        close(fd);
        rmdir(dir_path);
        return 1;
    }
    if (expect_named_event(fd, dir_wd, IN_CREATE, file_name) != 0) {
        close(file_fd);
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }
    if (expect_named_event(fd, dir_wd, IN_OPEN, file_name) != 0) {
        close(file_fd);
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }

    if (write(file_fd, "x", 1) != 1) {
        fprintf(stderr, "FAIL: write watched file: %s\n", strerror(errno));
        close(file_fd);
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }
    if (expect_named_event(fd, dir_wd, IN_MODIFY, file_name) != 0) {
        close(file_fd);
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }

    close(file_fd);
    if (expect_named_event(fd, dir_wd, IN_CLOSE_WRITE, file_name) != 0) {
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }

    int read_fd = open(file_path, O_RDONLY);
    if (read_fd < 0) {
        fprintf(stderr, "FAIL: open read-only watched file: %s\n", strerror(errno));
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }
    if (expect_named_event(fd, dir_wd, IN_OPEN, file_name) != 0) {
        close(read_fd);
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }
    close(read_fd);
    if (expect_named_event(fd, dir_wd, IN_CLOSE_NOWRITE, file_name) != 0) {
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }

    int opened_dir_fd = open(dir_path, O_RDONLY | O_DIRECTORY);
    if (opened_dir_fd < 0) {
        fprintf(stderr, "FAIL: open watched directory: %s\n", strerror(errno));
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }
    if (expect_unnamed_event(fd, dir_wd, IN_OPEN | IN_ISDIR) != 0) {
        close(opened_dir_fd);
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }
    close(opened_dir_fd);
    if (expect_unnamed_event(fd, dir_wd, IN_CLOSE_NOWRITE | IN_ISDIR) != 0) {
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }

    int file_wd = inotify_add_watch_raw(fd, file_path,
                                        IN_OPEN | IN_CLOSE_NOWRITE | IN_ATTRIB | IN_DELETE_SELF);
    if (file_wd < 0) {
        fprintf(stderr, "FAIL: inotify_add_watch file %s: %s\n", file_path, strerror(errno));
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }

    read_fd = open(file_path, O_RDONLY);
    if (read_fd < 0) {
        fprintf(stderr, "FAIL: open file watch read-only: %s\n", strerror(errno));
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }
    if (expect_named_and_unnamed_events(fd, dir_wd, IN_OPEN, file_name, file_wd, IN_OPEN) != 0) {
        close(read_fd);
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }
    close(read_fd);
    if (expect_named_and_unnamed_events(fd, dir_wd, IN_CLOSE_NOWRITE, file_name, file_wd,
                                        IN_CLOSE_NOWRITE) != 0) {
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }

    if (chmod(file_path, 0640) != 0) {
        fprintf(stderr, "FAIL: chmod watched file: %s\n", strerror(errno));
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }
    if (expect_named_and_unnamed_events(fd, dir_wd, IN_ATTRIB, file_name, file_wd, IN_ATTRIB) !=
        0) {
        close(fd);
        unlink(file_path);
        rmdir(dir_path);
        return 1;
    }

    if (unlink(file_path) != 0) {
        fprintf(stderr, "FAIL: unlink watched file: %s\n", strerror(errno));
        close(fd);
        rmdir(dir_path);
        return 1;
    }
    if (expect_named_event(fd, dir_wd, IN_DELETE, file_name) != 0) {
        close(fd);
        rmdir(dir_path);
        return 1;
    }
    if (expect_unnamed_event(fd, file_wd, IN_DELETE_SELF) != 0) {
        close(fd);
        rmdir(dir_path);
        return 1;
    }
    if (expect_unnamed_event(fd, file_wd, IN_IGNORED) != 0) {
        close(fd);
        rmdir(dir_path);
        return 1;
    }

    if (inotify_rm_watch_raw(fd, dir_wd) != 0) {
        fprintf(stderr, "FAIL: inotify_rm_watch dir: %s\n", strerror(errno));
        close(fd);
        rmdir(dir_path);
        return 1;
    }
    memset(&event, 0, sizeof(event));
    if (read(fd, &event, sizeof(event)) != (ssize_t)sizeof(event) || event.wd != dir_wd ||
        event.mask != IN_IGNORED) {
        fprintf(stderr, "FAIL: read inotify dir ignored event wd=%d mask=%#x errno=%s\n",
                event.wd, event.mask, strerror(errno));
        close(fd);
        rmdir(dir_path);
        return 1;
    }

    close(fd);
    rmdir(dir_path);
    printf("inotify init, add, mask-add, rm, poll, read and fs event tests passed\n");
    printf("All inotify smoke tests passed!\n");
    return 0;
}
