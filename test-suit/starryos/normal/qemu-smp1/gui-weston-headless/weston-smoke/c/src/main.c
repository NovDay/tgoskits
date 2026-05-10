#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <libinput.h>
#include <libudev.h>
#include "xdg-shell-client-protocol.h"

struct ClientState {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *xdg_wm_base;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_surface *surface;
    struct wl_buffer *buffer;
    void *buffer_pixels;
    size_t buffer_size;
    bool saw_compositor;
    bool saw_shm;
    bool saw_output;
    bool saw_xdg_wm_base;
    bool saw_argb8888;
    bool configured;
    uint32_t configure_serial;
    int32_t configured_width;
    int32_t configured_height;
    bool sync_done;
    bool frame_done;
};

static const char *WESTON_STDIO_LOG = "/tmp/weston.log";
static const char *WESTON_SOCKET_NAME = "weston-smoke";

#define DRM_IOCTL_MODE_GETRESOURCES 0xc04064a0
#define DRM_IOCTL_MODE_GETCRTC 0xc06864a1

struct DrmModeModeInfo {
    uint32_t clock;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
};

struct DrmModeCardRes {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
};

struct DrmModeCrtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct DrmModeModeInfo mode;
};

static int open_restricted(const char *path, int flags, void *user_data) {
    (void)user_data;
    return open(path, flags | O_NONBLOCK | O_CLOEXEC);
}

static void close_restricted(int fd, void *user_data) {
    (void)user_data;
    close(fd);
}

static const struct libinput_interface libinput_iface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

static void trace_step(const char *message) {
    printf("weston-smoke: %s\n", message);
    fflush(stdout);
}

static void fail_on_timeout(int signo) {
    (void)signo;
    const char message[] = "FAIL: Weston smoke timed out\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static void cleanup_child(pid_t pid) {
    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; i++) {
        int status = 0;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return;
        }
        usleep(20000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static int ensure_runtime_dir(void) {
    if (mkdir("/run/user", 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "FAIL: mkdir /run/user: %s\n", strerror(errno));
        return 1;
    }
    if (mkdir("/run/user/0", 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "FAIL: mkdir /run/user/0: %s\n", strerror(errno));
        return 1;
    }
    if (chmod("/run/user/0", 0700) != 0) {
        fprintf(stderr, "FAIL: chmod /run/user/0: %s\n", strerror(errno));
        return 1;
    }
    if (setenv("XDG_RUNTIME_DIR", "/run/user/0", 1) != 0) {
        fprintf(stderr, "FAIL: setenv XDG_RUNTIME_DIR: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static bool use_drm_backend(void) {
    const char *backend = getenv("STARRY_WESTON_BACKEND");
    return backend != NULL && strcmp(backend, "drm") == 0;
}

static bool use_weston_debug(void) {
    const char *value = getenv("STARRY_WESTON_DEBUG");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static bool require_drm_output(void) {
    const char *value = getenv("STARRY_WESTON_REQUIRE_OUTPUT");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static const char *weston_logger_scopes(void) {
    const char *value = getenv("STARRY_WESTON_LOGGER_SCOPES");
    return (value != NULL && value[0] != '\0') ? value : NULL;
}

static const char *weston_flight_rec_scopes(void) {
    const char *value = getenv("STARRY_WESTON_FLIGHT_REC_SCOPES");
    return (value != NULL && value[0] != '\0') ? value : NULL;
}

static int env_int_ms(const char *name, int default_value) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return default_value;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > 60000) {
        fprintf(stderr, "FAIL: invalid %s value '%s'\n", name, value);
        return -1;
    }
    return (int)parsed;
}

static int wait_for_child_alive(pid_t pid, int delay_ms, const char *phase) {
    if (delay_ms <= 0) {
        return 0;
    }
    printf("weston-smoke: wait %d ms before %s\n", delay_ms, phase);
    fflush(stdout);
    for (int waited = 0; waited < delay_ms; waited += 100) {
        int status = 0;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            if (WIFEXITED(status)) {
                fprintf(stderr, "FAIL: weston exited with status %d before %s\n",
                        WEXITSTATUS(status), phase);
            } else if (WIFSIGNALED(status)) {
                fprintf(stderr, "FAIL: weston terminated by signal %d before %s\n",
                        WTERMSIG(status), phase);
            } else {
                fprintf(stderr, "FAIL: weston stopped before %s\n", phase);
            }
            return 1;
        }
        int sleep_ms = delay_ms - waited;
        if (sleep_ms > 100) {
            sleep_ms = 100;
        }
        usleep((useconds_t)sleep_ms * 1000);
    }
    return 0;
}

static void dump_text_tail(const char *path, size_t max_bytes, const char *label) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        printf("weston-smoke: %s unavailable at %s: %s\n", label, path, strerror(errno));
        fflush(stdout);
        return;
    }

    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        printf("weston-smoke: failed to stat %s: %s\n", path, strerror(errno));
        fflush(stdout);
        close(fd);
        return;
    }

    off_t start = 0;
    if ((size_t)size > max_bytes) {
        start = size - (off_t)max_bytes;
    }
    if (lseek(fd, start, SEEK_SET) < 0) {
        printf("weston-smoke: failed to seek %s: %s\n", path, strerror(errno));
        fflush(stdout);
        close(fd);
        return;
    }

    char buffer[4096];
    printf("weston-smoke: --- %s tail (%s) ---\n", label, path);
    fflush(stdout);
    ssize_t len;
    while ((len = read(fd, buffer, sizeof(buffer))) > 0) {
        ssize_t written = write(STDOUT_FILENO, buffer, (size_t)len);
        (void)written;
    }
    if (len < 0) {
        printf("weston-smoke: failed to read %s: %s\n", path, strerror(errno));
    }
    printf("weston-smoke: --- end %s tail ---\n", label);
    fflush(stdout);
    close(fd);
}

static int redirect_child_stdio(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return 1;
    }
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        close(fd);
        return 1;
    }
    if (fd > STDERR_FILENO) {
        close(fd);
    }
    return 0;
}

static uint64_t hash_bytes(const uint8_t *data, size_t len) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static int sample_framebuffer(uint64_t *hash_out, size_t *bytes_out) {
    int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "FAIL: open /dev/fb0 for Weston output sample: %s\n", strerror(errno));
        return 1;
    }
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        fprintf(stderr, "FAIL: FBIOGET_FSCREENINFO for Weston output sample: %s\n",
                strerror(errno));
        close(fd);
        return 1;
    }
    if (fix.smem_len == 0) {
        fprintf(stderr, "FAIL: /dev/fb0 reports empty framebuffer\n");
        close(fd);
        return 1;
    }
    size_t sample_size = fix.smem_len;
    if (sample_size > 8 * 1024 * 1024) {
        sample_size = 8 * 1024 * 1024;
    }
    uint8_t *sample = malloc(sample_size);
    if (sample == NULL) {
        fprintf(stderr, "FAIL: allocate Weston output sample buffer\n");
        close(fd);
        return 1;
    }
    size_t done = 0;
    while (done < sample_size) {
        ssize_t n = pread(fd, sample + done, sample_size - done, (off_t)done);
        if (n < 0) {
            fprintf(stderr, "FAIL: read Weston output sample at %zu: %s\n", done,
                    strerror(errno));
            free(sample);
            close(fd);
            return 1;
        }
        if (n == 0) {
            fprintf(stderr, "FAIL: short Weston output sample read at %zu expected=%zu\n", done,
                    sample_size);
            free(sample);
            close(fd);
            return 1;
        }
        done += (size_t)n;
    }
    close(fd);
    *hash_out = hash_bytes(sample, sample_size);
    *bytes_out = sample_size;
    free(sample);
    return 0;
}

static int sample_drm_crtc(uint32_t *fb_id_out) {
    int fd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "FAIL: open /dev/dri/card0 for Weston CRTC sample: %s\n", strerror(errno));
        return 1;
    }

    struct DrmModeCardRes res = {0};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        fprintf(stderr, "FAIL: DRM_IOCTL_MODE_GETRESOURCES probe: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    if (res.count_crtcs < 1) {
        fprintf(stderr, "FAIL: DRM reports no CRTCs\n");
        close(fd);
        return 1;
    }
    uint32_t crtc_id = 0;
    res.crtc_id_ptr = (uintptr_t)&crtc_id;
    res.count_crtcs = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        fprintf(stderr, "FAIL: DRM_IOCTL_MODE_GETRESOURCES CRTC fetch: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    struct DrmModeCrtc crtc = {
        .crtc_id = crtc_id,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) != 0) {
        fprintf(stderr, "FAIL: DRM_IOCTL_MODE_GETCRTC probe: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    *fb_id_out = crtc.fb_id;
    return 0;
}

static int sample_output_state(const char *phase, uint64_t *fb_hash, uint32_t *crtc_fb_id) {
    size_t bytes = 0;
    if (sample_framebuffer(fb_hash, &bytes) != 0 || sample_drm_crtc(crtc_fb_id) != 0) {
        return 1;
    }
    printf("weston-smoke: output sample %s fb_hash=%#llx bytes=%zu crtc_fb_id=%u\n", phase,
           (unsigned long long)*fb_hash, bytes, *crtc_fb_id);
    fflush(stdout);
    return 0;
}

static void dump_child_state(pid_t pid, const char *label) {
    if (pid <= 0) {
        return;
    }
    if (kill(pid, 0) == 0) {
        printf("weston-smoke: %s pid=%d still alive\n", label, pid);
    } else {
        printf("weston-smoke: %s pid=%d not alive: %s\n", label, pid, strerror(errno));
    }
    fflush(stdout);
}

static int run_libinput_dispatch_child(void) {
    const char *event_paths[] = {"/dev/input/event0", "/dev/input/event1"};
    for (size_t i = 0; i < sizeof(event_paths) / sizeof(event_paths[0]); i++) {
        int evfd = open(event_paths[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (evfd < 0) {
            fprintf(stderr, "FAIL: libinput probe open %s: %s\n", event_paths[i],
                    strerror(errno));
            return 1;
        }
        struct input_event ev;
        int reads = 0;
        for (;;) {
            ssize_t n = read(evfd, &ev, sizeof(ev));
            if (n == (ssize_t)sizeof(ev)) {
                reads++;
                continue;
            }
            if (n < 0 && errno == EAGAIN) {
                printf("weston-smoke: direct evdev drain %s reads=%d -> EAGAIN\n",
                       event_paths[i], reads);
                fflush(stdout);
                break;
            }
            if (n < 0) {
                fprintf(stderr, "FAIL: direct evdev read %s: %s\n", event_paths[i],
                        strerror(errno));
            } else {
                fprintf(stderr, "FAIL: direct evdev short read %s: %zd\n", event_paths[i], n);
            }
            close(evfd);
            return 1;
        }
        close(evfd);
    }

    struct udev *udev = udev_new();
    if (udev == NULL) {
        fprintf(stderr, "FAIL: libinput probe udev_new returned NULL\n");
        return 1;
    }
    struct libinput *li = libinput_udev_create_context(&libinput_iface, NULL, udev);
    if (li == NULL) {
        fprintf(stderr, "FAIL: libinput probe create_context returned NULL\n");
        udev_unref(udev);
        return 1;
    }
    if (libinput_udev_assign_seat(li, "seat0") != 0) {
        fprintf(stderr, "FAIL: libinput probe assign_seat failed\n");
        libinput_unref(li);
        udev_unref(udev);
        return 1;
    }
    int fd = libinput_get_fd(li);
    int fd_flags_value = fcntl(fd, F_GETFL);
    char fd_target[128];
    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);
    ssize_t target_len = readlink(fd_path, fd_target, sizeof(fd_target) - 1);
    if (target_len >= 0) {
        fd_target[target_len] = '\0';
    } else {
        snprintf(fd_target, sizeof(fd_target), "readlink failed: %s", strerror(errno));
    }
    printf("weston-smoke: libinput dispatch child li_fd=%d flags=0x%x nonblock=%d target=%s\n",
           fd, fd_flags_value, fd_flags_value >= 0 && (fd_flags_value & O_NONBLOCK) != 0,
           fd_target);
    fflush(stdout);
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int poll_rc = poll(&pfd, 1, 250);
    printf("weston-smoke: libinput dispatch child fd=%d poll_rc=%d revents=0x%x\n", fd,
           poll_rc, pfd.revents);
    fflush(stdout);
    if (poll_rc < 0) {
        fprintf(stderr, "FAIL: libinput probe poll failed: %s\n", strerror(errno));
        libinput_unref(li);
        udev_unref(udev);
        return 1;
    }
    int dispatch_rc = libinput_dispatch(li);
    printf("weston-smoke: libinput dispatch child dispatch_rc=%d\n", dispatch_rc);
    fflush(stdout);
    int event_count = 0;
    for (;;) {
        struct libinput_event *event = libinput_get_event(li);
        if (event == NULL) {
            break;
        }
        enum libinput_event_type type = libinput_event_get_type(event);
        printf("weston-smoke: libinput dispatch child event type=%d\n", type);
        fflush(stdout);
        event_count++;
        libinput_event_destroy(event);
    }
    printf("weston-smoke: libinput dispatch child events=%d\n", event_count);
    fflush(stdout);
    libinput_unref(li);
    udev_unref(udev);
    return dispatch_rc == 0 ? 0 : 1;
}

static int probe_libinput_dispatch_timeout(void) {
    trace_step("probe libinput udev dispatch in child");
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "FAIL: fork libinput dispatch probe: %s\n", strerror(errno));
        return 1;
    }
    if (child == 0) {
        alarm(5);
        _exit(run_libinput_dispatch_child());
    }
    for (int i = 0; i < 60; i++) {
        int status = 0;
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            if (WIFEXITED(status)) {
                printf("weston-smoke: libinput dispatch probe exited status=%d\n",
                       WEXITSTATUS(status));
                fflush(stdout);
                return WEXITSTATUS(status) == 0 ? 0 : 1;
            }
            if (WIFSIGNALED(status)) {
                fprintf(stderr, "FAIL: libinput dispatch probe signal=%d\n", WTERMSIG(status));
                return 1;
            }
            fprintf(stderr, "FAIL: libinput dispatch probe ended unexpectedly\n");
            return 1;
        }
        usleep(100000);
    }
    fprintf(stderr, "FAIL: libinput dispatch probe did not return\n");
    cleanup_child(child);
    return 1;
}

static int create_cloexec_memfd(const char *name) {
#ifdef SYS_memfd_create
    int memfd = (int)syscall(SYS_memfd_create, name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memfd >= 0 || errno != ENOSYS) {
        return memfd;
    }
#endif

    char path[] = "/tmp/weston-smoke-shm-XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) {
        unlink(path);
        int flags = fcntl(fd, F_GETFD);
        if (flags >= 0) {
            (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        }
    }
    return fd;
}

static int start_weston(pid_t *weston_pid, bool drm_backend) {
    printf("weston-smoke: start Weston %s compositor\n", drm_backend ? "DRM" : "headless");
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "FAIL: fork weston: %s\n", strerror(errno));
        return 1;
    }

    if (pid == 0) {
        unlink(WESTON_STDIO_LOG);
        if (redirect_child_stdio(WESTON_STDIO_LOG) != 0) {
            fprintf(stderr, "FAIL: redirect weston stdio: %s\n", strerror(errno));
            _exit(126);
        }
        if (drm_backend) {
            (void)setenv("LIBSEAT_BACKEND", "noop", 1);
            const char *renderer = getenv("STARRY_WESTON_RENDERER");
            const char *logger_scopes = weston_logger_scopes();
            const char *flight_rec_scopes = weston_flight_rec_scopes();
            char renderer_arg[64];
            char logger_arg[256];
            char flight_rec_arg[256];
            const char *weston_argv[16];
            int weston_argc = 0;
            weston_argv[weston_argc++] = "/usr/bin/weston";
            weston_argv[weston_argc++] = "--backend=drm-backend.so";
            weston_argv[weston_argc++] = "--shell=kiosk-shell.so";
            weston_argv[weston_argc++] = "--socket=weston-smoke";
            weston_argv[weston_argc++] = "--idle-time=0";
            weston_argv[weston_argc++] = "--no-config";
            if (use_weston_debug()) {
                weston_argv[weston_argc++] = "--debug";
            }
            if (renderer != NULL && renderer[0] != '\0') {
                snprintf(renderer_arg, sizeof(renderer_arg), "--renderer=%s", renderer);
                weston_argv[weston_argc++] = renderer_arg;
            }
            if (logger_scopes != NULL) {
                snprintf(logger_arg, sizeof(logger_arg), "--logger-scopes=%s", logger_scopes);
                weston_argv[weston_argc++] = logger_arg;
            }
            if (flight_rec_scopes != NULL) {
                snprintf(flight_rec_arg, sizeof(flight_rec_arg), "--flight-rec-scopes=%s",
                         flight_rec_scopes);
                weston_argv[weston_argc++] = flight_rec_arg;
            }
            weston_argv[weston_argc] = NULL;

            execv("/usr/bin/weston", (char *const *)weston_argv);
            fprintf(stderr, "FAIL: exec /usr/bin/weston: %s\n", strerror(errno));
        } else {
            execl("/usr/bin/weston", "weston", "--backend=headless-backend.so",
                  "--shell=kiosk-shell.so", "--socket=weston-smoke", "--idle-time=0",
                  "--no-config", "--width=320", "--height=240", (char *)NULL);
            fprintf(stderr, "FAIL: exec /usr/bin/weston headless: %s\n", strerror(errno));
        }
        _exit(127);
    }

    *weston_pid = pid;
    return 0;
}

static void registry_global(
    void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version) {
    (void)registry;
    (void)name;
    (void)version;
    struct ClientState *state = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->saw_compositor = true;
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->saw_shm = true;
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->saw_xdg_wm_base = true;
        uint32_t bind_version = version < 2 ? version : 2;
        state->xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, bind_version);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        state->saw_output = true;
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void shm_format(void *data, struct wl_shm *shm, uint32_t format) {
    (void)shm;
    struct ClientState *state = data;
    if (format == WL_SHM_FORMAT_ARGB8888) {
        state->saw_argb8888 = true;
    }
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_format,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void xdg_surface_configure(
    void *data,
    struct xdg_surface *xdg_surface,
    uint32_t serial) {
    (void)xdg_surface;
    struct ClientState *state = data;
    state->configured = true;
    state->configure_serial = serial;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(
    void *data,
    struct xdg_toplevel *xdg_toplevel,
    int32_t width,
    int32_t height,
    struct wl_array *states) {
    (void)xdg_toplevel;
    (void)states;
    struct ClientState *state = data;
    state->configured_width = width;
    state->configured_height = height;
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    (void)data;
    (void)xdg_toplevel;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static void sync_done(void *data, struct wl_callback *callback, uint32_t callback_data) {
    (void)callback_data;
    struct ClientState *state = data;
    state->sync_done = true;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener sync_listener = {
    .done = sync_done,
};

static void frame_done(void *data, struct wl_callback *callback, uint32_t callback_data) {
    (void)callback_data;
    struct ClientState *state = data;
    state->frame_done = true;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static int roundtrip_with_timeout(struct ClientState *state) {
    const char *label = getenv("STARRY_WESTON_ROUNDTRIP_LABEL");
    if (label != NULL) {
        printf("weston-smoke: begin client roundtrip: %s\n", label);
        fflush(stdout);
    }
    state->sync_done = false;
    struct wl_callback *callback = wl_display_sync(state->display);
    if (callback == NULL) {
        fprintf(stderr, "FAIL: wl_display_sync returned NULL\n");
        return 1;
    }
    wl_callback_add_listener(callback, &sync_listener, state);

    if (wl_display_flush(state->display) < 0) {
        fprintf(stderr, "FAIL: wl_display_flush: %s\n", strerror(errno));
        return 1;
    }

    for (int i = 0; i < 150 && !state->sync_done; i++) {
        if (wl_display_dispatch_pending(state->display) < 0) {
            fprintf(stderr, "FAIL: wl_display_dispatch_pending: %s\n", strerror(errno));
            return 1;
        }
        if (state->sync_done) {
            break;
        }
        struct pollfd pfd = {.fd = wl_display_get_fd(state->display), .events = POLLIN};
        int ready = poll(&pfd, 1, 100);
        if (ready < 0) {
            fprintf(stderr, "FAIL: poll Wayland client fd: %s\n", strerror(errno));
            return 1;
        }
        if (ready == 0) {
            continue;
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            fprintf(stderr, "FAIL: Wayland client fd revents=0x%x\n", pfd.revents);
            return 1;
        }
        if ((pfd.revents & POLLIN) != 0 && wl_display_dispatch(state->display) < 0) {
            fprintf(stderr, "FAIL: wl_display_dispatch: %s\n", strerror(errno));
            return 1;
        }
    }

    if (!state->sync_done) {
        fprintf(stderr, "FAIL: Weston client roundtrip did not complete");
        if (label != NULL) {
            fprintf(stderr, " during %s", label);
        }
        fprintf(stderr, "\n");
        return 1;
    }
    if (label != NULL) {
        printf("weston-smoke: completed client roundtrip: %s\n", label);
        fflush(stdout);
    }
    return 0;
}

static int wait_for_frame_callback(struct ClientState *state, bool require_frame) {
    state->frame_done = false;
    struct wl_callback *callback = wl_surface_frame(state->surface);
    if (callback == NULL) {
        fprintf(stderr, "FAIL: wl_surface_frame returned NULL\n");
        return 1;
    }
    wl_callback_add_listener(callback, &frame_listener, state);
    wl_surface_commit(state->surface);
    if (wl_display_flush(state->display) < 0) {
        fprintf(stderr, "FAIL: wl_display_flush frame callback: %s\n", strerror(errno));
        return 1;
    }

    for (int i = 0; i < 150 && !state->frame_done; i++) {
        if (wl_display_dispatch_pending(state->display) < 0) {
            fprintf(stderr, "FAIL: wl_display_dispatch_pending frame callback: %s\n",
                    strerror(errno));
            return 1;
        }
        if (state->frame_done) {
            break;
        }
        struct pollfd pfd = {.fd = wl_display_get_fd(state->display), .events = POLLIN};
        int ready = poll(&pfd, 1, 100);
        if (ready < 0) {
            fprintf(stderr, "FAIL: poll Wayland client fd for frame callback: %s\n",
                    strerror(errno));
            return 1;
        }
        if (ready == 0) {
            continue;
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            fprintf(stderr, "FAIL: Wayland client fd frame callback revents=0x%x\n", pfd.revents);
            return 1;
        }
        if ((pfd.revents & POLLIN) != 0 && wl_display_dispatch(state->display) < 0) {
            fprintf(stderr, "FAIL: wl_display_dispatch frame callback: %s\n", strerror(errno));
            return 1;
        }
    }

    if (!state->frame_done) {
        printf("weston-smoke: Weston frame callback did not complete after surface commit\n");
        fflush(stdout);
        if (require_frame) {
            fprintf(stderr, "FAIL: Weston frame callback did not complete after surface commit\n");
            return 1;
        }
        return 0;
    }
    printf("weston-smoke: Weston frame callback completed after surface commit\n");
    fflush(stdout);
    return 0;
}

static int create_client_buffer(struct ClientState *state) {
    const int width = 32;
    const int height = 32;
    const int stride = width * 4;
    const size_t size = (size_t)stride * (size_t)height;

    int fd = create_cloexec_memfd("weston-smoke-buffer");
    if (fd < 0) {
        fprintf(stderr, "FAIL: create Weston shm fd: %s\n", strerror(errno));
        return 1;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        fprintf(stderr, "FAIL: ftruncate Weston shm fd: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        fprintf(stderr, "FAIL: mmap Weston shm buffer: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    uint32_t *argb = pixels;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t red = (uint32_t)(x * 255 / (width - 1));
            uint32_t green = (uint32_t)(y * 255 / (height - 1));
            argb[y * width + x] = 0xff000000u | (red << 16) | (green << 8) | 0x40u;
        }
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, (int32_t)size);
    if (pool == NULL) {
        fprintf(stderr, "FAIL: wl_shm_create_pool returned NULL\n");
        munmap(pixels, size);
        close(fd);
        return 1;
    }

    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (buffer == NULL) {
        fprintf(stderr, "FAIL: wl_shm_pool_create_buffer returned NULL\n");
        munmap(pixels, size);
        return 1;
    }

    state->buffer = buffer;
    state->buffer_pixels = pixels;
    state->buffer_size = size;
    return 0;
}

static int submit_shm_surface(struct ClientState *state) {
    state->surface = wl_compositor_create_surface(state->compositor);
    if (state->surface == NULL) {
        fprintf(stderr, "FAIL: wl_compositor_create_surface returned NULL\n");
        return 1;
    }
    if (state->xdg_wm_base == NULL) {
        fprintf(stderr, "FAIL: missing xdg_wm_base for Weston surface role\n");
        return 1;
    }
    state->xdg_surface = xdg_wm_base_get_xdg_surface(state->xdg_wm_base, state->surface);
    if (state->xdg_surface == NULL) {
        fprintf(stderr, "FAIL: xdg_wm_base_get_xdg_surface returned NULL\n");
        return 1;
    }
    xdg_surface_add_listener(state->xdg_surface, &xdg_surface_listener, state);
    state->xdg_toplevel = xdg_surface_get_toplevel(state->xdg_surface);
    if (state->xdg_toplevel == NULL) {
        fprintf(stderr, "FAIL: xdg_surface_get_toplevel returned NULL\n");
        return 1;
    }
    xdg_toplevel_add_listener(state->xdg_toplevel, &xdg_toplevel_listener, state);
    xdg_toplevel_set_title(state->xdg_toplevel, "starry-weston-smoke");
    xdg_toplevel_set_app_id(state->xdg_toplevel, "starry-weston-smoke");
    xdg_toplevel_set_fullscreen(state->xdg_toplevel, NULL);
    wl_surface_commit(state->surface);
    (void)setenv("STARRY_WESTON_ROUNDTRIP_LABEL", "xdg configure", 1);
    if (roundtrip_with_timeout(state) != 0 || !state->configured) {
        fprintf(stderr, "FAIL: Weston did not configure xdg toplevel\n");
        return 1;
    }
    printf("weston-smoke: xdg toplevel configured width=%d height=%d serial=%u\n",
           state->configured_width, state->configured_height, state->configure_serial);
    fflush(stdout);
    xdg_surface_ack_configure(state->xdg_surface, state->configure_serial);

    if (create_client_buffer(state) != 0) {
        return 1;
    }

    wl_surface_attach(state->surface, state->buffer, 0, 0);
    wl_surface_damage_buffer(state->surface, 0, 0, 32, 32);
    wl_surface_commit(state->surface);
    (void)setenv("STARRY_WESTON_ROUNDTRIP_LABEL", "surface commit", 1);
    if (roundtrip_with_timeout(state) != 0) {
        return 1;
    }
    if (wait_for_frame_callback(state, use_drm_backend() || require_drm_output()) != 0) {
        return 1;
    }
    return 0;
}

static int check_output_after_surface_commit(
    uint64_t before_hash,
    uint32_t before_crtc_fb_id,
    bool require_change) {
    uint64_t after_hash = 0;
    uint32_t after_crtc_fb_id = 0;

    usleep(200000);
    if (sample_output_state("after surface commit", &after_hash, &after_crtc_fb_id) != 0) {
        return 1;
    }

    bool changed = after_hash != before_hash || after_crtc_fb_id != before_crtc_fb_id;
    if (changed) {
        printf("weston-smoke: Weston DRM output changed after surface commit\n");
        fflush(stdout);
        return 0;
    }

    printf("weston-smoke: Weston DRM output unchanged after surface commit "
           "fb_hash=%#llx crtc_fb_id=%u\n",
           (unsigned long long)after_hash, after_crtc_fb_id);
    fflush(stdout);
    if (require_change) {
        fprintf(stderr, "FAIL: Weston DRM output did not change after surface commit\n");
        return 1;
    }
    return 0;
}

static int connect_to_weston(struct ClientState *state, pid_t weston_pid) {
    int delay_ms = env_int_ms("STARRY_WESTON_CONNECT_DELAY_MS", 0);
    if (delay_ms < 0 || wait_for_child_alive(weston_pid, delay_ms, "client connect") != 0) {
        return 1;
    }

    int retry_ms = env_int_ms("STARRY_WESTON_CONNECT_RETRY_MS", use_drm_backend() ? 15000 : 5000);
    if (retry_ms < 0) {
        return 1;
    }
    int max_attempts = retry_ms / 100;
    if (max_attempts < 1) {
        max_attempts = 1;
    }

    printf("weston-smoke: retry client connect for up to %d ms\n", retry_ms);
    fflush(stdout);
    for (int i = 0; i < max_attempts; i++) {
        state->display = wl_display_connect(WESTON_SOCKET_NAME);
        if (state->display != NULL) {
            int client_fd = wl_display_get_fd(state->display);
            int flags = fcntl(client_fd, F_GETFL);
            if (flags >= 0) {
                printf("weston-smoke: weston client fd=%d flags=0x%x nonblock=%d\n", client_fd,
                       flags, (flags & O_NONBLOCK) != 0);
                fflush(stdout);
            }
            return 0;
        }
        int status = 0;
        pid_t done = waitpid(weston_pid, &status, WNOHANG);
        if (done == weston_pid) {
            if (WIFEXITED(status)) {
                fprintf(stderr, "FAIL: weston exited with status %d before accepting clients\n",
                        WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                fprintf(stderr, "FAIL: weston terminated by signal %d before accepting clients\n",
                        WTERMSIG(status));
            } else {
                fprintf(stderr, "FAIL: weston stopped before accepting clients\n");
            }
            return 1;
        }
        usleep(100000);
    }
    fprintf(stderr, "FAIL: wl_display_connect weston-smoke: %s\n", strerror(errno));
    return 1;
}

int main(void) {
    signal(SIGALRM, fail_on_timeout);
    alarm(30);

    if (ensure_runtime_dir() != 0) {
        return 1;
    }
    if (use_drm_backend() && probe_libinput_dispatch_timeout() != 0) {
        return 1;
    }

    bool drm_backend = use_drm_backend();
    pid_t weston_pid = -1;
    if (start_weston(&weston_pid, drm_backend) != 0) {
        return 1;
    }

    struct ClientState state = {0};
    int result = 1;
    if (connect_to_weston(&state, weston_pid) != 0) {
        goto out;
    }
    state.registry = wl_display_get_registry(state.display);
    if (state.registry == NULL) {
        fprintf(stderr, "FAIL: wl_display_get_registry returned NULL\n");
        goto out;
    }
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    (void)setenv("STARRY_WESTON_ROUNDTRIP_LABEL", "registry globals", 1);
    if (roundtrip_with_timeout(&state) != 0) {
        goto out;
    }
    if (state.shm != NULL) {
        wl_shm_add_listener(state.shm, &shm_listener, &state);
    }
    if (state.xdg_wm_base != NULL) {
        xdg_wm_base_add_listener(state.xdg_wm_base, &xdg_wm_base_listener, &state);
    }
    (void)setenv("STARRY_WESTON_ROUNDTRIP_LABEL", "wl_shm formats", 1);
    if (roundtrip_with_timeout(&state) != 0) {
        goto out;
    }
    if (!state.saw_compositor || state.compositor == NULL || !state.saw_shm ||
        state.shm == NULL || !state.saw_output || !state.saw_xdg_wm_base ||
        state.xdg_wm_base == NULL || !state.saw_argb8888) {
        fprintf(stderr,
                "FAIL: Weston globals missing compositor=%d shm=%d output=%d xdg=%d "
                "argb8888=%d\n",
                state.saw_compositor, state.saw_shm, state.saw_output, state.saw_xdg_wm_base,
                state.saw_argb8888);
        goto out;
    }
    uint64_t before_output_hash = 0;
    uint32_t before_crtc_fb_id = 0;
    bool sampled_output = false;
    if (drm_backend) {
        if (sample_output_state("before surface commit", &before_output_hash, &before_crtc_fb_id) !=
            0) {
            goto out;
        }
        sampled_output = true;
    }
    if (submit_shm_surface(&state) != 0) {
        goto out;
    }
    if (sampled_output &&
        check_output_after_surface_commit(before_output_hash, before_crtc_fb_id, true) != 0) {
        goto out;
    }

    printf("Weston %s compositor accepted a Wayland shm surface commit\n",
           drm_backend ? "DRM" : "headless");
    printf("All Weston smoke tests passed!\n");
    result = 0;

out:
    if (result != 0) {
        dump_child_state(weston_pid, "weston");
        dump_text_tail(WESTON_STDIO_LOG, 8192, "weston log");
    }
    if (state.buffer != NULL) {
        wl_buffer_destroy(state.buffer);
    }
    if (state.buffer_pixels != NULL && state.buffer_size > 0) {
        munmap(state.buffer_pixels, state.buffer_size);
    }
    if (state.xdg_toplevel != NULL) {
        xdg_toplevel_destroy(state.xdg_toplevel);
    }
    if (state.xdg_surface != NULL) {
        xdg_surface_destroy(state.xdg_surface);
    }
    if (state.surface != NULL) {
        wl_surface_destroy(state.surface);
    }
    if (state.xdg_wm_base != NULL) {
        xdg_wm_base_destroy(state.xdg_wm_base);
    }
    if (state.shm != NULL) {
        wl_shm_destroy(state.shm);
    }
    if (state.compositor != NULL) {
        wl_compositor_destroy(state.compositor);
    }
    if (state.registry != NULL) {
        wl_registry_destroy(state.registry);
    }
    if (state.display != NULL) {
        wl_display_disconnect(state.display);
    }
    cleanup_child(weston_pid);
    return result;
}
