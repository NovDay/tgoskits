#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib-unix.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct SmokeState {
    GMainLoop *loop;
    GSource *fd_source;
    int pipe_fds[2];
    gboolean timeout_seen;
    gboolean fd_seen;
    gboolean idle_seen;
};

static gboolean timeout_cb(gpointer data) {
    struct SmokeState *state = data;
    state->timeout_seen = TRUE;
    const char byte = 'g';
    if (write(state->pipe_fds[1], &byte, 1) != 1) {
        g_printerr("FAIL: GLib timeout pipe write: %s\n", g_strerror(errno));
        g_main_loop_quit(state->loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_REMOVE;
}

static gboolean idle_cb(gpointer data) {
    struct SmokeState *state = data;
    state->idle_seen = TRUE;
    return G_SOURCE_REMOVE;
}

static gboolean fd_cb(gint fd, GIOCondition condition, gpointer data) {
    struct SmokeState *state = data;
    if ((condition & G_IO_IN) == 0) {
        g_printerr("FAIL: GLib fd source condition=%#x\n", condition);
        g_main_loop_quit(state->loop);
        return G_SOURCE_REMOVE;
    }

    char byte = 0;
    if (read(fd, &byte, 1) != 1 || byte != 'g') {
        g_printerr("FAIL: GLib fd source read byte=%c errno=%s\n", byte, g_strerror(errno));
        g_main_loop_quit(state->loop);
        return G_SOURCE_REMOVE;
    }

    state->fd_seen = TRUE;
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        g_printerr("FAIL: F_GETFL pipe fd: %s\n", g_strerror(errno));
        return 1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        g_printerr("FAIL: F_SETFL pipe fd O_NONBLOCK: %s\n", g_strerror(errno));
        return 1;
    }
    return 0;
}

int main(void) {
    struct SmokeState state = {
        .loop = NULL,
        .fd_source = NULL,
        .pipe_fds = {-1, -1},
        .timeout_seen = FALSE,
        .fd_seen = FALSE,
        .idle_seen = FALSE,
    };

    if (pipe(state.pipe_fds) != 0) {
        g_printerr("FAIL: pipe: %s\n", g_strerror(errno));
        return 1;
    }
    if (set_nonblocking(state.pipe_fds[0]) != 0) {
        close(state.pipe_fds[0]);
        close(state.pipe_fds[1]);
        return 1;
    }

    state.loop = g_main_loop_new(NULL, FALSE);
    if (state.loop == NULL) {
        g_printerr("FAIL: g_main_loop_new returned NULL\n");
        close(state.pipe_fds[0]);
        close(state.pipe_fds[1]);
        return 1;
    }

    state.fd_source = g_unix_fd_source_new(state.pipe_fds[0], G_IO_IN);
    if (state.fd_source == NULL) {
        g_printerr("FAIL: g_unix_fd_source_new returned NULL\n");
        g_main_loop_unref(state.loop);
        close(state.pipe_fds[0]);
        close(state.pipe_fds[1]);
        return 1;
    }
    g_source_set_callback(state.fd_source, (GSourceFunc)fd_cb, &state, NULL);
    g_source_attach(state.fd_source, NULL);
    g_source_unref(state.fd_source);

    g_idle_add(idle_cb, &state);
    g_timeout_add(10, timeout_cb, &state);
    g_main_loop_run(state.loop);

    if (!state.idle_seen || !state.timeout_seen || !state.fd_seen) {
        g_printerr("FAIL: GLib main loop incomplete idle=%d timeout=%d fd=%d\n",
                   state.idle_seen, state.timeout_seen, state.fd_seen);
        g_main_loop_unref(state.loop);
        close(state.pipe_fds[0]);
        close(state.pipe_fds[1]);
        return 1;
    }

    GFile *file = g_file_new_for_path("/proc/self/fd");
    if (file == NULL) {
        g_printerr("FAIL: g_file_new_for_path returned NULL\n");
        g_main_loop_unref(state.loop);
        close(state.pipe_fds[0]);
        close(state.pipe_fds[1]);
        return 1;
    }
    char *path = g_file_get_path(file);
    if (path == NULL || strcmp(path, "/proc/self/fd") != 0) {
        g_printerr("FAIL: GFile path=%s\n", path == NULL ? "(null)" : path);
        g_free(path);
        g_object_unref(file);
        g_main_loop_unref(state.loop);
        close(state.pipe_fds[0]);
        close(state.pipe_fds[1]);
        return 1;
    }
    g_free(path);
    g_object_unref(file);

    g_main_loop_unref(state.loop);
    close(state.pipe_fds[0]);
    close(state.pipe_fds[1]);

    g_print("GLib main loop, timeout, idle, fd source and GFile tests passed\n");
    g_print("All GLib smoke tests passed!\n");
    return 0;
}
