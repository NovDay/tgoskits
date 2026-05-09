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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <linux/falloc.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif

#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#endif

#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK 0x0002
#endif

#ifndef F_SEAL_GROW
#define F_SEAL_GROW 0x0004
#endif

#ifndef F_SEAL_WRITE
#define F_SEAL_WRITE 0x0008
#endif

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;
    uint32_t width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

struct fb_fix_screeninfo {
    uint8_t id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

struct ServerState {
    struct wl_display *display;
    struct wl_global *compositor_global;
    struct wl_event_source *listener_source;
    int listener_fd;
    int fb_fd;
    uint8_t *fb;
    struct fb_var_screeninfo fb_var;
    struct fb_fix_screeninfo fb_fix;
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct wl_resource *attached_buffer;
    bool surface_created;
    bool buffer_attached;
    bool surface_committed;
    bool framebuffer_blitted;
    bool surface_destroyed;
};

struct ClientState {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    bool saw_compositor;
    bool saw_shm;
};

static void fail_on_timeout(int signo) {
    (void)signo;
    const char message[] = "FAIL: Wayland process smoke timed out\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static void trace_step(const char *message) {
    printf("wayland-process-smoke: %s\n", message);
    fflush(stdout);
}

static int create_cloexec_memfd(const char *name) {
#ifdef SYS_memfd_create
    int memfd = (int)syscall(SYS_memfd_create, name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memfd >= 0 || errno != ENOSYS) {
        return memfd;
    }
#endif

    char path[] = "/tmp/wayland-process-shm-XXXXXX";
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

static uint32_t pack_fb_rgb(const struct fb_var_screeninfo *var, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t pixel = 0;
    pixel |= ((uint32_t)r >> (8 - var->red.length)) << var->red.offset;
    pixel |= ((uint32_t)g >> (8 - var->green.length)) << var->green.offset;
    pixel |= ((uint32_t)b >> (8 - var->blue.length)) << var->blue.offset;
    if (var->transp.length > 0) {
        pixel |= ((1u << var->transp.length) - 1u) << var->transp.offset;
    }
    return pixel;
}

static bool valid_color_field(const struct fb_bitfield *field, uint32_t bits_per_pixel) {
    return field->length > 0 && field->length <= 8 && field->offset < bits_per_pixel &&
           field->offset + field->length <= bits_per_pixel;
}

static void put_fb_pixel(struct ServerState *state, uint32_t x, uint32_t y, uint32_t pixel) {
    size_t offset =
        (size_t)y * state->fb_fix.line_length + (size_t)x * (state->fb_var.bits_per_pixel / 8);
    memcpy(state->fb + offset, &pixel, state->fb_var.bits_per_pixel / 8);
}

static uint32_t get_fb_pixel(const struct ServerState *state, uint32_t x, uint32_t y) {
    uint32_t pixel = 0;
    size_t offset =
        (size_t)y * state->fb_fix.line_length + (size_t)x * (state->fb_var.bits_per_pixel / 8);
    memcpy(&pixel, state->fb + offset, state->fb_var.bits_per_pixel / 8);
    return pixel;
}

static int setup_framebuffer(struct ServerState *state) {
    state->fb_fd = open("/dev/fb0", O_RDWR);
    if (state->fb_fd < 0) {
        fprintf(stderr, "FAIL: open /dev/fb0 for Wayland blit: %s\n", strerror(errno));
        return 1;
    }
    if (ioctl(state->fb_fd, FBIOGET_VSCREENINFO, &state->fb_var) != 0) {
        fprintf(stderr, "FAIL: FBIOGET_VSCREENINFO for Wayland blit: %s\n", strerror(errno));
        return 1;
    }
    if (ioctl(state->fb_fd, FBIOGET_FSCREENINFO, &state->fb_fix) != 0) {
        fprintf(stderr, "FAIL: FBIOGET_FSCREENINFO for Wayland blit: %s\n", strerror(errno));
        return 1;
    }
    if (state->fb_var.xres < 8 || state->fb_var.yres < 8 || state->fb_fix.smem_len == 0 ||
        state->fb_fix.line_length == 0 ||
        (state->fb_var.bits_per_pixel != 24 && state->fb_var.bits_per_pixel != 32)) {
        fprintf(stderr, "FAIL: unsupported framebuffer for Wayland blit %ux%u@%u line=%u\n",
                state->fb_var.xres, state->fb_var.yres, state->fb_var.bits_per_pixel,
                state->fb_fix.line_length);
        return 1;
    }
    if (!valid_color_field(&state->fb_var.red, state->fb_var.bits_per_pixel) ||
        !valid_color_field(&state->fb_var.green, state->fb_var.bits_per_pixel) ||
        !valid_color_field(&state->fb_var.blue, state->fb_var.bits_per_pixel) ||
        state->fb_var.transp.length > 8 ||
        state->fb_var.transp.offset + state->fb_var.transp.length > state->fb_var.bits_per_pixel) {
        fprintf(stderr, "FAIL: unsupported framebuffer bitfields r=%u:%u g=%u:%u b=%u:%u a=%u:%u\n",
                state->fb_var.red.offset, state->fb_var.red.length, state->fb_var.green.offset,
                state->fb_var.green.length, state->fb_var.blue.offset, state->fb_var.blue.length,
                state->fb_var.transp.offset, state->fb_var.transp.length);
        return 1;
    }
    state->fb =
        mmap(NULL, state->fb_fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, state->fb_fd, 0);
    if (state->fb == MAP_FAILED) {
        fprintf(stderr, "FAIL: mmap /dev/fb0 for Wayland blit: %s\n", strerror(errno));
        state->fb = NULL;
        return 1;
    }
    return 0;
}

static int blit_shm_buffer_to_framebuffer(struct ServerState *state) {
    if (state->attached_buffer == NULL) {
        fprintf(stderr, "FAIL: commit without attached Wayland buffer\n");
        return 1;
    }
    struct wl_shm_buffer *shm_buffer = wl_shm_buffer_get(state->attached_buffer);
    if (shm_buffer == NULL) {
        fprintf(stderr, "FAIL: attached Wayland buffer is not wl_shm\n");
        return 1;
    }
    if (wl_shm_buffer_get_format(shm_buffer) != WL_SHM_FORMAT_ARGB8888 ||
        wl_shm_buffer_get_width(shm_buffer) < 8 || wl_shm_buffer_get_height(shm_buffer) < 8) {
        fprintf(stderr, "FAIL: unexpected wl_shm buffer format=%u size=%dx%d\n",
                wl_shm_buffer_get_format(shm_buffer), wl_shm_buffer_get_width(shm_buffer),
                wl_shm_buffer_get_height(shm_buffer));
        return 1;
    }

    wl_shm_buffer_begin_access(shm_buffer);
    const uint8_t *src = wl_shm_buffer_get_data(shm_buffer);
    int stride = wl_shm_buffer_get_stride(shm_buffer);
    for (uint32_t y = 0; y < 8; y++) {
        const uint32_t *row = (const uint32_t *)(src + (size_t)y * (size_t)stride);
        for (uint32_t x = 0; x < 8; x++) {
            uint32_t argb = row[x];
            uint8_t r = (uint8_t)((argb >> 16) & 0xff);
            uint8_t g = (uint8_t)((argb >> 8) & 0xff);
            uint8_t b = (uint8_t)(argb & 0xff);
            put_fb_pixel(state, x, y, pack_fb_rgb(&state->fb_var, r, g, b));
        }
    }
    wl_shm_buffer_end_access(shm_buffer);

    uint32_t expected = pack_fb_rgb(&state->fb_var, 7, 7, 0);
    uint32_t observed = get_fb_pixel(state, 7, 7);
    if (observed != expected) {
        fprintf(stderr, "FAIL: Wayland framebuffer blit readback got=%#x expected=%#x\n",
                observed, expected);
        return 1;
    }
    state->framebuffer_blitted = true;
    return 0;
}

static void surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    struct ServerState *state = wl_resource_get_user_data(resource);
    state->surface_destroyed = true;
    wl_resource_destroy(resource);
}

static void surface_attach(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *buffer,
    int32_t x,
    int32_t y) {
    (void)client;
    (void)x;
    (void)y;
    struct ServerState *state = wl_resource_get_user_data(resource);
    state->buffer_attached = buffer != NULL;
    state->attached_buffer = buffer;
}

static void surface_damage(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height) {
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct wl_resource *callback =
        wl_resource_create(client, &wl_callback_interface, wl_resource_get_version(resource), id);
    if (callback != NULL) {
        wl_callback_send_done(callback, 1);
        wl_resource_destroy(callback);
    }
}

static void surface_set_opaque_region(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *region) {
    (void)client;
    (void)resource;
    (void)region;
}

static void surface_set_input_region(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *region) {
    (void)client;
    (void)resource;
    (void)region;
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    struct ServerState *state = wl_resource_get_user_data(resource);
    state->surface_committed = true;
    if (blit_shm_buffer_to_framebuffer(state) != 0) {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "failed to blit shm buffer to framebuffer");
    }
}

static void surface_set_buffer_transform(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t transform) {
    (void)client;
    (void)resource;
    (void)transform;
}

static void surface_set_buffer_scale(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t scale) {
    (void)client;
    (void)resource;
    (void)scale;
}

static void surface_damage_buffer(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height) {
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
};

static void compositor_create_surface(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id) {
    struct ServerState *state = wl_resource_get_user_data(resource);
    struct wl_resource *surface =
        wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(resource), id);
    if (surface == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    state->surface_created = true;
    wl_resource_set_implementation(surface, &surface_impl, state, NULL);
}

static void region_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void region_add(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height) {
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void region_subtract(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height) {
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static const struct wl_region_interface region_impl = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_subtract,
};

static void compositor_create_region(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id) {
    struct wl_resource *region =
        wl_resource_create(client, &wl_region_interface, wl_resource_get_version(resource), id);
    if (region == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(region, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void bind_compositor(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id) {
    struct ServerState *state = data;
    uint32_t bind_version = version < 4 ? version : 4;
    struct wl_resource *resource =
        wl_resource_create(client, &wl_compositor_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &compositor_impl, state, NULL);
}

static int accept_wayland_client(int fd, uint32_t mask, void *data) {
    struct ServerState *state = data;

    if ((mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) != 0) {
        fprintf(stderr, "FAIL: listener event mask=0x%x\n", mask);
        return 0;
    }
    if ((mask & WL_EVENT_READABLE) == 0) {
        return 0;
    }

    for (;;) {
        int client_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            fprintf(stderr, "FAIL: accept Wayland process client: %s\n", strerror(errno));
            return 0;
        }
        if (wl_client_create(state->display, client_fd) == NULL) {
            close(client_fd);
            fprintf(stderr, "FAIL: wl_client_create returned NULL\n");
            return 0;
        }
    }
}

static int setup_server(struct ServerState *state) {
    state->listener_fd = -1;
    state->fb_fd = -1;
    if (setup_framebuffer(state) != 0) {
        return 1;
    }
    state->display = wl_display_create();
    if (state->display == NULL) {
        fprintf(stderr, "FAIL: wl_display_create returned NULL\n");
        return 1;
    }
    if (wl_display_init_shm(state->display) != 0) {
        fprintf(stderr, "FAIL: wl_display_init_shm: %s\n", strerror(errno));
        return 1;
    }

    state->compositor_global =
        wl_global_create(state->display, &wl_compositor_interface, 4, state, bind_compositor);
    if (state->compositor_global == NULL) {
        fprintf(stderr, "FAIL: wl_global_create compositor returned NULL\n");
        return 1;
    }

    state->listener_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (state->listener_fd < 0) {
        fprintf(stderr, "FAIL: create listener socket: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(state->socket_path, sizeof(state->socket_path), "/tmp/wayland-process-%u.sock",
             (unsigned)getpid());
    strncpy(addr.sun_path, state->socket_path, sizeof(addr.sun_path) - 1);
    unlink(state->socket_path);

    if (bind(state->listener_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "FAIL: bind listener socket: %s\n", strerror(errno));
        return 1;
    }
    if (listen(state->listener_fd, 128) != 0) {
        fprintf(stderr, "FAIL: listen listener socket: %s\n", strerror(errno));
        return 1;
    }

    state->listener_source = wl_event_loop_add_fd(
        wl_display_get_event_loop(state->display), state->listener_fd, WL_EVENT_READABLE,
        accept_wayland_client, state);
    if (state->listener_source == NULL) {
        fprintf(stderr, "FAIL: wl_event_loop_add_fd returned NULL\n");
        return 1;
    }
    return 0;
}

static void cleanup_server(struct ServerState *state) {
    if (state->display != NULL) {
        wl_display_destroy_clients(state->display);
    }
    if (state->listener_source != NULL) {
        wl_event_source_remove(state->listener_source);
    }
    if (state->compositor_global != NULL) {
        wl_global_destroy(state->compositor_global);
    }
    if (state->display != NULL) {
        wl_display_destroy(state->display);
    }
    if (state->listener_fd >= 0) {
        close(state->listener_fd);
    }
    if (state->fb != NULL) {
        munmap(state->fb, state->fb_fix.smem_len);
    }
    if (state->fb_fd >= 0) {
        close(state->fb_fd);
    }
    if (state->socket_path[0] != '\0') {
        unlink(state->socket_path);
    }
}

static void registry_global(
    void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version) {
    struct ClientState *state = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        uint32_t bind_version = version < 4 ? version : 4;
        state->compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, bind_version);
        state->saw_compositor = true;
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        state->saw_shm = true;
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

static int expect_errno(const char *operation, int expected_errno) {
    if (errno == expected_errno) {
        return 0;
    }
    fprintf(stderr, "FAIL: %s errno=%s expected=%s\n", operation, strerror(errno),
            strerror(expected_errno));
    return 1;
}

static int validate_memfd_seals(void) {
    const size_t size = 4096;
    int fd = create_cloexec_memfd("wayland-process-seal-check");
    if (fd < 0) {
        fprintf(stderr, "FAIL: create seal-check memfd: %s\n", strerror(errno));
        return 1;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        fprintf(stderr, "FAIL: ftruncate seal-check memfd: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
        fprintf(stderr, "FAIL: add grow/shrink seals: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    errno = 0;
    if (ftruncate(fd, (off_t)(size / 2)) == 0 ||
        expect_errno("F_SEAL_SHRINK ftruncate", EPERM) != 0) {
        close(fd);
        return 1;
    }
    errno = 0;
    if (ftruncate(fd, (off_t)(size * 2)) == 0 ||
        expect_errno("F_SEAL_GROW ftruncate", EPERM) != 0) {
        close(fd);
        return 1;
    }
    errno = 0;
    if (fallocate(fd, 0, (off_t)size, (off_t)size) == 0 ||
        expect_errno("F_SEAL_GROW fallocate", EPERM) != 0) {
        close(fd);
        return 1;
    }

    const char before_write_seal = 'x';
    if (pwrite(fd, &before_write_seal, sizeof(before_write_seal), 0) !=
        (ssize_t)sizeof(before_write_seal)) {
        fprintf(stderr, "FAIL: pwrite before F_SEAL_WRITE: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    errno = 0;
    if (pwrite(fd, &before_write_seal, sizeof(before_write_seal), (off_t)size) == 0 ||
        expect_errno("F_SEAL_GROW pwrite", EPERM) != 0) {
        close(fd);
        return 1;
    }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) != 0) {
        fprintf(stderr, "FAIL: add write seal: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    const char after_write_seal = 'y';
    errno = 0;
    if (pwrite(fd, &after_write_seal, sizeof(after_write_seal), 0) == 0 ||
        expect_errno("F_SEAL_WRITE pwrite", EPERM) != 0) {
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int create_shm_buffer(
    struct ClientState *state,
    struct wl_buffer **out_buffer,
    void **out_pixels,
    size_t *out_size) {
    const int width = 8;
    const int height = 8;
    const int stride = width * 4;
    const size_t size = (size_t)stride * (size_t)height;

    if (validate_memfd_seals() != 0) {
        return 1;
    }

    int fd = create_cloexec_memfd("wayland-process-smoke");
    if (fd < 0) {
        fprintf(stderr, "FAIL: create shm fd: %s\n", strerror(errno));
        return 1;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        fprintf(stderr, "FAIL: ftruncate shm fd: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    int initial_seals = fcntl(fd, F_GET_SEALS);
    if (initial_seals != 0) {
        fprintf(stderr, "FAIL: initial memfd seals=%#x errno=%s\n", initial_seals,
                strerror(errno));
        close(fd);
        return 1;
    }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
        fprintf(stderr, "FAIL: F_ADD_SEALS memfd: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    int seals = fcntl(fd, F_GET_SEALS);
    if ((seals & (F_SEAL_SHRINK | F_SEAL_GROW)) != (F_SEAL_SHRINK | F_SEAL_GROW)) {
        fprintf(stderr, "FAIL: memfd seals after add=%#x errno=%s\n", seals, strerror(errno));
        close(fd);
        return 1;
    }

    void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        fprintf(stderr, "FAIL: mmap shm buffer: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    uint32_t *argb = pixels;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            argb[y * width + x] = 0xff000000u | ((uint32_t)x << 16) | ((uint32_t)y << 8);
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

    *out_buffer = buffer;
    *out_pixels = pixels;
    *out_size = size;
    return 0;
}

static void cleanup_client(struct ClientState *state) {
    if (state->compositor != NULL) {
        wl_compositor_destroy(state->compositor);
    }
    if (state->shm != NULL) {
        wl_shm_destroy(state->shm);
    }
    if (state->registry != NULL) {
        wl_registry_destroy(state->registry);
    }
    if (state->display != NULL) {
        wl_display_disconnect(state->display);
    }
}

static int run_client(const char *socket_path) {
    signal(SIGALRM, fail_on_timeout);
    alarm(8);

    struct ClientState state = {0};
    state.display = wl_display_connect(socket_path);
    if (state.display == NULL) {
        fprintf(stderr, "FAIL: wl_display_connect client: %s\n", strerror(errno));
        return 1;
    }

    state.registry = wl_display_get_registry(state.display);
    if (state.registry == NULL) {
        fprintf(stderr, "FAIL: wl_display_get_registry returned NULL\n");
        cleanup_client(&state);
        return 1;
    }
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    if (wl_display_roundtrip(state.display) < 0) {
        fprintf(stderr, "FAIL: registry roundtrip: %s\n", strerror(errno));
        cleanup_client(&state);
        return 1;
    }
    if (!state.saw_compositor || state.compositor == NULL || !state.saw_shm ||
        state.shm == NULL) {
        fprintf(stderr, "FAIL: globals compositor=%d shm=%d\n", state.saw_compositor,
                state.saw_shm);
        cleanup_client(&state);
        return 1;
    }

    struct wl_surface *surface = wl_compositor_create_surface(state.compositor);
    if (surface == NULL) {
        fprintf(stderr, "FAIL: wl_compositor_create_surface returned NULL\n");
        cleanup_client(&state);
        return 1;
    }

    struct wl_buffer *buffer = NULL;
    void *pixels = NULL;
    size_t pixels_size = 0;
    if (create_shm_buffer(&state, &buffer, &pixels, &pixels_size) != 0) {
        wl_surface_destroy(surface);
        cleanup_client(&state);
        return 1;
    }

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, 8, 8);
    wl_surface_commit(surface);
    if (wl_display_roundtrip(state.display) < 0) {
        fprintf(stderr, "FAIL: commit roundtrip: %s\n", strerror(errno));
        wl_buffer_destroy(buffer);
        munmap(pixels, pixels_size);
        wl_surface_destroy(surface);
        cleanup_client(&state);
        return 1;
    }

    wl_buffer_destroy(buffer);
    munmap(pixels, pixels_size);
    wl_surface_destroy(surface);
    if (wl_display_roundtrip(state.display) < 0) {
        fprintf(stderr, "FAIL: destroy roundtrip: %s\n", strerror(errno));
        cleanup_client(&state);
        return 1;
    }

    cleanup_client(&state);
    alarm(0);
    return 0;
}

static int run_server_until_child_exit(struct ServerState *state, pid_t child) {
    struct wl_event_loop *loop = wl_display_get_event_loop(state->display);

    for (;;) {
        wl_event_loop_dispatch(loop, 20);
        wl_display_flush_clients(state->display);

        int status = 0;
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == 0) {
            continue;
        }
        if (done != child) {
            fprintf(stderr, "FAIL: waitpid client: %s\n", strerror(errno));
            return 1;
        }
        wl_event_loop_dispatch(loop, 0);
        wl_display_flush_clients(state->display);

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "FAIL: client exited status=0x%x\n", status);
            return 1;
        }
        return 0;
    }
}

int main(void) {
    signal(SIGALRM, fail_on_timeout);
    alarm(12);

    struct ServerState server = {0};
    if (setup_server(&server) != 0) {
        cleanup_server(&server);
        return 1;
    }

    trace_step("fork client process");
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "FAIL: fork client: %s\n", strerror(errno));
        cleanup_server(&server);
        return 1;
    }
    if (child == 0) {
        int rc = run_client(server.socket_path);
        _exit(rc == 0 ? 0 : 1);
    }

    int rc = run_server_until_child_exit(&server, child);
    if (rc == 0 &&
        (!server.surface_created || !server.buffer_attached || !server.surface_committed ||
         !server.framebuffer_blitted || !server.surface_destroyed)) {
        fprintf(stderr,
                "FAIL: lifecycle created=%d attached=%d committed=%d blitted=%d destroyed=%d\n",
                server.surface_created, server.buffer_attached, server.surface_committed,
                server.framebuffer_blitted, server.surface_destroyed);
        rc = 1;
    }

    cleanup_server(&server);
    alarm(0);
    if (rc != 0) {
        return 1;
    }

    printf("Wayland independent process shm surface lifecycle and framebuffer blit tests passed\n");
    printf("All Wayland process smoke tests passed!\n");
    return 0;
}
