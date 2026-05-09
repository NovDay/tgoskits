#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>

struct TestState {
    struct wl_display *server_display;
    struct wl_global *compositor_global;
    struct wl_display *client_display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_surface *surface;
    struct wl_event_source *listener_source;
    int server_fd;
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    bool saw_compositor;
    bool client_sync_done;
    bool server_surface_created;
    bool server_surface_committed;
    bool server_surface_destroyed;
};

static void fail_on_timeout(int signo) {
    (void)signo;
    const char message[] = "FAIL: Wayland smoke timed out\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static void trace_step(const char *message) {
    printf("wayland-smoke: %s\n", message);
    fflush(stdout);
}

static int pump_roundtrip(struct TestState *state) {
    trace_step("flush client requests");
    if (wl_display_flush(state->client_display) < 0) {
        fprintf(stderr, "FAIL: wl_display_flush: %s\n", strerror(errno));
        return 1;
    }
    trace_step("dispatch server event loop");
    if (wl_event_loop_dispatch(wl_display_get_event_loop(state->server_display), 0) < 0) {
        fprintf(stderr, "FAIL: wl_event_loop_dispatch: %s\n", strerror(errno));
        return 1;
    }
    trace_step("flush server clients");
    wl_display_flush_clients(state->server_display);

    trace_step("prepare client read");
    while (wl_display_prepare_read(state->client_display) != 0) {
        if (wl_display_dispatch_pending(state->client_display) < 0) {
            fprintf(stderr, "FAIL: wl_display_dispatch_pending: %s\n", strerror(errno));
            return 1;
        }
    }
    trace_step("flush client before read");
    if (wl_display_flush(state->client_display) < 0) {
        wl_display_cancel_read(state->client_display);
        fprintf(stderr, "FAIL: wl_display_flush before read: %s\n", strerror(errno));
        return 1;
    }

    struct pollfd pfd = {
        .fd = wl_display_get_fd(state->client_display),
        .events = POLLIN,
    };
    trace_step("poll client fd");
    int poll_result = poll(&pfd, 1, 100);
    if (poll_result < 0) {
        wl_display_cancel_read(state->client_display);
        fprintf(stderr, "FAIL: poll Wayland client fd: %s\n", strerror(errno));
        return 1;
    }
    if (poll_result == 0) {
        wl_display_cancel_read(state->client_display);
        return 0;
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        wl_display_cancel_read(state->client_display);
        fprintf(stderr, "FAIL: Wayland client fd revents=0x%x\n", pfd.revents);
        return 1;
    }
    if ((pfd.revents & POLLIN) == 0) {
        wl_display_cancel_read(state->client_display);
        return 0;
    }
    trace_step("read client events");
    if (wl_display_read_events(state->client_display) < 0) {
        fprintf(stderr, "FAIL: wl_display_read_events: %s\n", strerror(errno));
        return 1;
    }
    trace_step("dispatch pending client events");
    if (wl_display_dispatch_pending(state->client_display) < 0) {
        fprintf(stderr, "FAIL: wl_display_dispatch_pending after read: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static void sync_done(void *data, struct wl_callback *callback, uint32_t callback_data) {
    (void)callback_data;
    struct TestState *state = data;
    state->client_sync_done = true;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener sync_listener = {
    .done = sync_done,
};

static int client_roundtrip(struct TestState *state) {
    state->client_sync_done = false;
    trace_step("create sync callback");
    struct wl_callback *callback = wl_display_sync(state->client_display);
    if (callback == NULL) {
        fprintf(stderr, "FAIL: wl_display_sync returned NULL\n");
        return 1;
    }
    wl_callback_add_listener(callback, &sync_listener, state);

    for (int i = 0; i < 8 && !state->client_sync_done; i++) {
        if (pump_roundtrip(state) != 0) {
            return 1;
        }
    }
    if (!state->client_sync_done) {
        fprintf(stderr, "FAIL: Wayland roundtrip did not complete\n");
        return 1;
    }
    return 0;
}

static void surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    struct TestState *state = wl_resource_get_user_data(resource);
    state->server_surface_destroyed = true;
    wl_resource_destroy(resource);
}

static void surface_attach(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *buffer,
    int32_t x,
    int32_t y) {
    (void)client;
    (void)resource;
    (void)buffer;
    (void)x;
    (void)y;
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
    struct TestState *state = wl_resource_get_user_data(resource);
    state->server_surface_committed = true;
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
    struct TestState *state = wl_resource_get_user_data(resource);
    struct wl_resource *surface =
        wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(resource), id);
    if (surface == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(surface, &surface_impl, state, NULL);
    state->server_surface_created = true;
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
    struct TestState *state = data;
    struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &compositor_impl, state, NULL);
}

static void registry_global(
    void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version) {
    struct TestState *state = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->saw_compositor = true;
        uint32_t bind_version = version < 4 ? version : 4;
        state->compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, bind_version);
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

static int probe_auto_socket(void) {
    trace_step("probe auto Wayland display socket");
    if (setenv("XDG_RUNTIME_DIR", "/tmp", 1) != 0) {
        fprintf(stderr, "FAIL: setenv XDG_RUNTIME_DIR for auto probe: %s\n", strerror(errno));
        return 1;
    }
    struct wl_display *display = wl_display_create();
    if (display == NULL) {
        fprintf(stderr, "FAIL: wl_display_create auto probe returned NULL\n");
        return 1;
    }
    const char *socket_name = wl_display_add_socket_auto(display);
    if (socket_name == NULL) {
        fprintf(stderr, "FAIL: wl_display_add_socket_auto probe: %s\n", strerror(errno));
        wl_display_destroy(display);
        return 1;
    }
    struct wl_display *client = wl_display_connect(socket_name);
    if (client == NULL) {
        fprintf(stderr, "FAIL: wl_display_connect auto probe: %s\n", strerror(errno));
        wl_display_destroy(display);
        return 1;
    }
    wl_display_disconnect(client);
    wl_display_destroy(display);
    return 0;
}

static int accept_wayland_client(int fd, uint32_t mask, void *data) {
    struct TestState *state = data;

    if ((mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) != 0) {
        fprintf(stderr, "FAIL: Wayland listener event mask=0x%x\n", mask);
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
            fprintf(stderr, "FAIL: accept Wayland client: %s\n", strerror(errno));
            return 0;
        }

        struct wl_client *client = wl_client_create(state->server_display, client_fd);
        if (client == NULL) {
            close(client_fd);
            fprintf(stderr, "FAIL: wl_client_create returned NULL\n");
            return 0;
        }
    }
}

static int setup_wayland_pair(struct TestState *state) {
    trace_step("set runtime dir");
    if (setenv("XDG_RUNTIME_DIR", "/tmp", 1) != 0) {
        fprintf(stderr, "FAIL: setenv XDG_RUNTIME_DIR: %s\n", strerror(errno));
        return 1;
    }

    trace_step("create server display");
    state->server_display = wl_display_create();
    if (state->server_display == NULL) {
        fprintf(stderr, "FAIL: wl_display_create returned NULL\n");
        return 1;
    }

    trace_step("create compositor global");
    state->compositor_global = wl_global_create(
        state->server_display, &wl_compositor_interface, 4, state, bind_compositor);
    if (state->compositor_global == NULL) {
        fprintf(stderr, "FAIL: wl_global_create compositor returned NULL\n");
        return 1;
    }

    trace_step("create nonblocking server socket");
    state->server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (state->server_fd < 0) {
        fprintf(stderr, "FAIL: Wayland listener socket: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(state->socket_path, sizeof(state->socket_path), "/tmp/wayland-smoke-%u.sock",
             (unsigned)getpid());
    strncpy(addr.sun_path, state->socket_path, sizeof(addr.sun_path) - 1);
    unlink(state->socket_path);

    if (bind(state->server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "FAIL: bind Wayland listener: %s\n", strerror(errno));
        return 1;
    }
    if (listen(state->server_fd, 128) != 0) {
        fprintf(stderr, "FAIL: listen Wayland listener: %s\n", strerror(errno));
        return 1;
    }

    trace_step("add server listener source");
    state->listener_source = wl_event_loop_add_fd(
        wl_display_get_event_loop(state->server_display), state->server_fd, WL_EVENT_READABLE,
        accept_wayland_client, state);
    if (state->listener_source == NULL) {
        fprintf(stderr, "FAIL: wl_event_loop_add_fd listener returned NULL\n");
        return 1;
    }

    trace_step("connect client display to server socket");
    state->client_display = wl_display_connect(state->socket_path);
    if (state->client_display == NULL) {
        fprintf(stderr, "FAIL: wl_display_connect returned NULL: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static void cleanup_wayland_pair(struct TestState *state) {
    if (state->surface != NULL) {
        wl_surface_destroy(state->surface);
    }
    if (state->compositor != NULL) {
        wl_compositor_destroy(state->compositor);
    }
    if (state->registry != NULL) {
        wl_registry_destroy(state->registry);
    }
    if (state->client_display != NULL) {
        wl_display_disconnect(state->client_display);
    }
    if (state->server_display != NULL) {
        wl_display_destroy_clients(state->server_display);
    }
    if (state->listener_source != NULL) {
        wl_event_source_remove(state->listener_source);
    }
    if (state->compositor_global != NULL) {
        wl_global_destroy(state->compositor_global);
    }
    if (state->server_display != NULL) {
        wl_display_destroy(state->server_display);
    }
    if (state->server_fd >= 0) {
        close(state->server_fd);
    }
    if (state->socket_path[0] != '\0') {
        unlink(state->socket_path);
    }
}

int main(void) {
    signal(SIGALRM, fail_on_timeout);
    alarm(10);

    if (probe_auto_socket() != 0) {
        return 1;
    }

    struct TestState state = {.server_fd = -1};
    if (setup_wayland_pair(&state) != 0) {
        cleanup_wayland_pair(&state);
        return 1;
    }

    trace_step("get client registry");
    state.registry = wl_display_get_registry(state.client_display);
    if (state.registry == NULL) {
        fprintf(stderr, "FAIL: wl_display_get_registry returned NULL\n");
        cleanup_wayland_pair(&state);
        return 1;
    }
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    trace_step("roundtrip registry");
    if (client_roundtrip(&state) != 0) {
        cleanup_wayland_pair(&state);
        return 1;
    }
    if (!state.saw_compositor || state.compositor == NULL) {
        fprintf(stderr, "FAIL: Wayland registry did not expose wl_compositor\n");
        cleanup_wayland_pair(&state);
        return 1;
    }

    trace_step("create surface");
    state.surface = wl_compositor_create_surface(state.compositor);
    if (state.surface == NULL) {
        fprintf(stderr, "FAIL: wl_compositor_create_surface returned NULL\n");
        cleanup_wayland_pair(&state);
        return 1;
    }
    trace_step("commit surface");
    wl_surface_damage_buffer(state.surface, 0, 0, 1, 1);
    wl_surface_commit(state.surface);
    if (client_roundtrip(&state) != 0) {
        cleanup_wayland_pair(&state);
        return 1;
    }
    if (!state.server_surface_created || !state.server_surface_committed) {
        fprintf(stderr, "FAIL: Wayland surface lifecycle created=%d committed=%d\n",
                state.server_surface_created, state.server_surface_committed);
        cleanup_wayland_pair(&state);
        return 1;
    }

    trace_step("destroy surface");
    wl_surface_destroy(state.surface);
    state.surface = NULL;
    if (client_roundtrip(&state) != 0) {
        cleanup_wayland_pair(&state);
        return 1;
    }
    if (!state.server_surface_destroyed) {
        fprintf(stderr, "FAIL: Wayland surface destroy was not observed by server\n");
        cleanup_wayland_pair(&state);
        return 1;
    }

    cleanup_wayland_pair(&state);
    alarm(0);
    printf("Wayland server/client socket registry, roundtrip and surface tests passed\n");
    printf("All Wayland smoke tests passed!\n");
    return 0;
}
