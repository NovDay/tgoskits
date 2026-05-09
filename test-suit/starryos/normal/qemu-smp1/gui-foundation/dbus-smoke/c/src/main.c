#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int read_line_with_timeout(int fd, char *buf, size_t cap, int timeout_ms) {
    size_t used = 0;
    while (used + 1 < cap) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pfd, 1, timeout_ms);
        if (ready <= 0) {
            return -1;
        }
        char ch = 0;
        ssize_t n = read(fd, &ch, 1);
        if (n != 1) {
            return -1;
        }
        if (ch == '\n') {
            buf[used] = '\0';
            return 0;
        }
        buf[used++] = ch;
    }
    errno = ENOSPC;
    return -1;
}

static void cleanup_daemon(pid_t pid) {
    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        int status = 0;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return;
        }
        usleep(10000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static int start_session_bus(char *address, size_t address_cap, pid_t *daemon_pid) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        fprintf(stderr, "FAIL: pipe for dbus-daemon: %s\n", strerror(errno));
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "FAIL: fork dbus-daemon: %s\n", strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return 1;
    }

    if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        set_cloexec(pipe_fds[1]);
        execl("/usr/bin/dbus-daemon", "dbus-daemon", "--session", "--nofork", "--print-address",
              (char *)NULL);
        _exit(127);
    }

    close(pipe_fds[1]);
    if (read_line_with_timeout(pipe_fds[0], address, address_cap, 5000) != 0) {
        fprintf(stderr, "FAIL: read dbus-daemon address: %s\n", strerror(errno));
        close(pipe_fds[0]);
        cleanup_daemon(pid);
        return 1;
    }
    close(pipe_fds[0]);

    if (strncmp(address, "unix:", 5) != 0) {
        fprintf(stderr, "FAIL: dbus-daemon address is not unix: %s\n", address);
        cleanup_daemon(pid);
        return 1;
    }

    *daemon_pid = pid;
    return 0;
}

static int call_list_names(DBusConnection *conn) {
    DBusError error;
    dbus_error_init(&error);

    DBusMessage *msg = dbus_message_new_method_call("org.freedesktop.DBus",
                                                    "/org/freedesktop/DBus",
                                                    "org.freedesktop.DBus", "ListNames");
    if (msg == NULL) {
        fprintf(stderr, "FAIL: dbus_message_new_method_call ListNames returned NULL\n");
        return 1;
    }

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &error);
    dbus_message_unref(msg);
    if (reply == NULL) {
        fprintf(stderr, "FAIL: D-Bus ListNames call: %s: %s\n", error.name, error.message);
        dbus_error_free(&error);
        return 1;
    }

    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        fprintf(stderr, "FAIL: D-Bus ListNames reply is not an array\n");
        dbus_message_unref(reply);
        return 1;
    }

    DBusMessageIter array;
    dbus_message_iter_recurse(&iter, &array);
    bool saw_bus_name = false;
    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING) {
        const char *name = NULL;
        dbus_message_iter_get_basic(&array, &name);
        if (name != NULL && strcmp(name, "org.freedesktop.DBus") == 0) {
            saw_bus_name = true;
            break;
        }
        dbus_message_iter_next(&array);
    }

    dbus_message_unref(reply);
    if (!saw_bus_name) {
        fprintf(stderr, "FAIL: D-Bus ListNames did not include org.freedesktop.DBus\n");
        return 1;
    }

    return 0;
}

static DBusHandlerResult fd_echo_handler(DBusConnection *conn, DBusMessage *msg, void *user_data) {
    bool *handled = user_data;
    if (!dbus_message_is_method_call(msg, "org.starryos.DBusSmoke", "EchoUnixFd")) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UNIX_FD) {
        DBusMessage *error = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
                                                    "expected a Unix fd argument");
        if (error != NULL) {
            dbus_connection_send(conn, error, NULL);
            dbus_message_unref(error);
        }
        *handled = true;
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    int received_fd = -1;
    dbus_message_iter_get_basic(&iter, &received_fd);
    char probe = 0;
    if (read(received_fd, &probe, 1) != 0) {
        DBusMessage *error =
            dbus_message_new_error(msg, DBUS_ERROR_FAILED, "received fd was not /dev/null");
        if (error != NULL) {
            dbus_connection_send(conn, error, NULL);
            dbus_message_unref(error);
        }
        *handled = true;
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    int reply_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (reply_fd < 0) {
        DBusMessage *error = dbus_message_new_error(msg, DBUS_ERROR_FAILED, "open /dev/null");
        if (error != NULL) {
            dbus_connection_send(conn, error, NULL);
            dbus_message_unref(error);
        }
        *handled = true;
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (reply == NULL ||
        !dbus_message_append_args(reply, DBUS_TYPE_UNIX_FD, &reply_fd, DBUS_TYPE_INVALID) ||
        !dbus_connection_send(conn, reply, NULL)) {
        close(reply_fd);
        if (reply != NULL) {
            dbus_message_unref(reply);
        }
        *handled = true;
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    close(reply_fd);
    *handled = true;
    return DBUS_HANDLER_RESULT_HANDLED;
}

static int run_fd_echo_server(const char *address, int ready_fd) {
    DBusError error;
    dbus_error_init(&error);
    DBusConnection *conn = dbus_connection_open_private(address, &error);
    if (conn == NULL) {
        fprintf(stderr, "FAIL: server dbus_connection_open_private: %s: %s\n", error.name,
                error.message);
        dbus_error_free(&error);
        return 1;
    }
    if (!dbus_bus_register(conn, &error)) {
        fprintf(stderr, "FAIL: server dbus_bus_register: %s: %s\n", error.name, error.message);
        dbus_error_free(&error);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return 1;
    }
    if (!dbus_connection_can_send_type(conn, DBUS_TYPE_UNIX_FD)) {
        fprintf(stderr, "FAIL: server connection cannot send Unix fds\n");
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return 1;
    }

    bool handled = false;
    static const DBusObjectPathVTable vtable = {
        .unregister_function = NULL,
        .message_function = fd_echo_handler,
    };
    if (!dbus_connection_register_object_path(conn, "/org/starryos/DBusSmoke", &vtable,
                                              &handled)) {
        fprintf(stderr, "FAIL: register D-Bus fd echo object path\n");
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return 1;
    }
    int request = dbus_bus_request_name(conn, "org.starryos.DBusSmoke",
                                        DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (request != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "FAIL: request D-Bus smoke name: %s: %s reply=%d\n", error.name,
                error.message, request);
        dbus_error_free(&error);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return 1;
    }

    if (write(ready_fd, "R", 1) != 1) {
        fprintf(stderr, "FAIL: notify D-Bus fd echo readiness: %s\n", strerror(errno));
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return 1;
    }
    close(ready_fd);

    while (!handled) {
        dbus_connection_read_write_dispatch(conn, 5000);
    }

    dbus_bus_release_name(conn, "org.starryos.DBusSmoke", NULL);
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    return 0;
}

static int call_fd_echo(const char *address) {
    int ready[2];
    if (pipe(ready) != 0) {
        fprintf(stderr, "FAIL: pipe for D-Bus fd echo server: %s\n", strerror(errno));
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "FAIL: fork D-Bus fd echo server: %s\n", strerror(errno));
        close(ready[0]);
        close(ready[1]);
        return 1;
    }
    if (pid == 0) {
        close(ready[0]);
        _exit(run_fd_echo_server(address, ready[1]));
    }
    close(ready[1]);

    char marker = 0;
    struct pollfd pfd = {.fd = ready[0], .events = POLLIN};
    int ready_ret = poll(&pfd, 1, 5000);
    if (ready_ret != 1 || read(ready[0], &marker, 1) != 1 || marker != 'R') {
        fprintf(stderr, "FAIL: wait for D-Bus fd echo server readiness: %s\n", strerror(errno));
        close(ready[0]);
        cleanup_daemon(pid);
        return 1;
    }
    close(ready[0]);

    DBusError error;
    dbus_error_init(&error);
    DBusConnection *conn = dbus_connection_open_private(address, &error);
    if (conn == NULL) {
        fprintf(stderr, "FAIL: client dbus_connection_open_private: %s: %s\n", error.name,
                error.message);
        dbus_error_free(&error);
        cleanup_daemon(pid);
        return 1;
    }
    if (!dbus_bus_register(conn, &error)) {
        fprintf(stderr, "FAIL: client dbus_bus_register: %s: %s\n", error.name, error.message);
        dbus_error_free(&error);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        cleanup_daemon(pid);
        return 1;
    }
    if (!dbus_connection_can_send_type(conn, DBUS_TYPE_UNIX_FD)) {
        fprintf(stderr, "FAIL: client connection cannot send Unix fds\n");
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        cleanup_daemon(pid);
        return 1;
    }

    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "FAIL: open /dev/null for D-Bus fd call: %s\n", strerror(errno));
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        cleanup_daemon(pid);
        return 1;
    }

    DBusMessage *msg = dbus_message_new_method_call("org.starryos.DBusSmoke",
                                                    "/org/starryos/DBusSmoke",
                                                    "org.starryos.DBusSmoke", "EchoUnixFd");
    if (msg == NULL ||
        !dbus_message_append_args(msg, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID)) {
        fprintf(stderr, "FAIL: create D-Bus Unix fd method call\n");
        close(fd);
        if (msg != NULL) {
            dbus_message_unref(msg);
        }
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        cleanup_daemon(pid);
        return 1;
    }

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &error);
    dbus_message_unref(msg);
    close(fd);
    if (reply == NULL) {
        fprintf(stderr, "FAIL: D-Bus Unix fd method call: %s: %s\n", error.name, error.message);
        dbus_error_free(&error);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        cleanup_daemon(pid);
        return 1;
    }

    int returned_fd = -1;
    if (!dbus_message_get_args(reply, &error, DBUS_TYPE_UNIX_FD, &returned_fd,
                               DBUS_TYPE_INVALID)) {
        fprintf(stderr, "FAIL: D-Bus Unix fd reply args: %s: %s\n", error.name, error.message);
        dbus_error_free(&error);
        dbus_message_unref(reply);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        cleanup_daemon(pid);
        return 1;
    }
    dbus_message_unref(reply);

    char probe = 0;
    ssize_t n = read(returned_fd, &probe, 1);
    close(returned_fd);
    if (n != 0) {
        fprintf(stderr, "FAIL: returned D-Bus fd is not /dev/null ret=%zd errno=%s\n", n,
                strerror(errno));
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        cleanup_daemon(pid);
        return 1;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: D-Bus fd echo server status=%d errno=%s\n", status,
                strerror(errno));
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return 1;
    }

    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    return 0;
}

int main(void) {
    errno = 0;
    if (mkdir("/tmp", 01777) != 0 && errno != EEXIST) {
        fprintf(stderr, "FAIL: mkdir /tmp: %s\n", strerror(errno));
        return 1;
    }
    setenv("DBUS_SESSION_BUS_ADDRESS", "", 1);

    char address[512];
    pid_t daemon_pid = -1;
    if (start_session_bus(address, sizeof(address), &daemon_pid) != 0) {
        return 1;
    }
    setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);

    DBusError error;
    dbus_error_init(&error);
    DBusConnection *conn = dbus_connection_open_private(address, &error);
    if (conn == NULL) {
        fprintf(stderr, "FAIL: dbus_connection_open_private: %s: %s\n", error.name,
                error.message);
        dbus_error_free(&error);
        cleanup_daemon(daemon_pid);
        return 1;
    }

    if (!dbus_bus_register(conn, &error)) {
        fprintf(stderr, "FAIL: dbus_bus_register: %s: %s\n", error.name, error.message);
        dbus_error_free(&error);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        cleanup_daemon(daemon_pid);
        return 1;
    }
    const char *unique_name = dbus_bus_get_unique_name(conn);
    if (unique_name == NULL || unique_name[0] != ':') {
        fprintf(stderr, "FAIL: unexpected D-Bus unique name after register: %s\n",
                unique_name == NULL ? "(null)" : unique_name);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        cleanup_daemon(daemon_pid);
        return 1;
    }

    int result = 0;
    if (call_list_names(conn) != 0) {
        result = 1;
    }
    if (result == 0 && call_fd_echo(address) != 0) {
        result = 1;
    }

    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    cleanup_daemon(daemon_pid);

    if (result != 0) {
        return result;
    }

    printf("D-Bus session daemon, Unix socket connection, method calls and Unix fd passing passed\n");
    printf("All D-Bus smoke tests passed!\n");
    return 0;
}
