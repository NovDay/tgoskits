#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

struct SeenInput {
    bool event0;
    bool event1;
};

static void trace_step(const char *message) {
    printf("udev-input-smoke: %s\n", message);
    fflush(stdout);
}

static void fail_on_timeout(int signo) {
    (void)signo;
    const char message[] = "FAIL: udev input smoke timed out\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

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

static int check_devnum_matches(const char *devnode, dev_t udev_devnum) {
    struct stat st;
    if (stat(devnode, &st) != 0) {
        fprintf(stderr, "FAIL: stat %s: %s\n", devnode, strerror(errno));
        return 1;
    }
    if (!S_ISCHR(st.st_mode)) {
        fprintf(stderr, "FAIL: %s is not a character device mode=%#o\n", devnode,
                (unsigned)st.st_mode);
        return 1;
    }
    if (st.st_rdev != udev_devnum) {
        fprintf(stderr, "FAIL: %s devnum mismatch stat=%#lx udev=%#lx\n", devnode,
                (unsigned long)st.st_rdev, (unsigned long)udev_devnum);
        return 1;
    }
    return 0;
}

static int check_udev_input_device(struct udev_device *device, struct SeenInput *seen) {
    const char *subsystem = udev_device_get_subsystem(device);
    const char *sysname = udev_device_get_sysname(device);
    const char *devnode = udev_device_get_devnode(device);
    const char *name = udev_device_get_sysattr_value(device, "name");

    if (subsystem == NULL || strcmp(subsystem, "input") != 0 || sysname == NULL) {
        return 0;
    }
    if (strcmp(sysname, "event0") != 0 && strcmp(sysname, "event1") != 0) {
        return 0;
    }
    if (devnode == NULL) {
        fprintf(stderr, "FAIL: udev input %s has no devnode\n", sysname);
        return 1;
    }
    if (name == NULL || strcmp(name, sysname) != 0) {
        fprintf(stderr, "FAIL: udev input %s name=%s\n", sysname, name == NULL ? "(null)" : name);
        return 1;
    }
    if (check_devnum_matches(devnode, udev_device_get_devnum(device)) != 0) {
        return 1;
    }
    if (udev_device_get_is_initialized(device) <= 0) {
        fprintf(stderr, "FAIL: udev input %s is not initialized\n", sysname);
        return 1;
    }
    const char *id_input = udev_device_get_property_value(device, "ID_INPUT");
    if (id_input == NULL || strcmp(id_input, "1") != 0) {
        fprintf(stderr, "FAIL: udev input %s ID_INPUT=%s\n", sysname,
                id_input == NULL ? "(null)" : id_input);
        return 1;
    }
    if (udev_device_has_tag(device, "seat") <= 0 || udev_device_has_tag(device, "seat0") <= 0) {
        fprintf(stderr, "FAIL: udev input %s is missing seat tags\n", sysname);
        return 1;
    }
    struct udev_device *parent = udev_device_get_parent(device);
    const char *expected_parent = strcmp(sysname, "event0") == 0 ? "input0" : "input1";
    const char *parent_sysname = parent == NULL ? NULL : udev_device_get_sysname(parent);
    const char *parent_subsystem = parent == NULL ? NULL : udev_device_get_subsystem(parent);
    if (parent_sysname == NULL || strcmp(parent_sysname, expected_parent) != 0 ||
        parent_subsystem == NULL || strcmp(parent_subsystem, "input") != 0) {
        fprintf(stderr, "FAIL: udev input %s parent sysname=%s subsystem=%s\n", sysname,
                parent_sysname == NULL ? "(null)" : parent_sysname,
                parent_subsystem == NULL ? "(null)" : parent_subsystem);
        return 1;
    }

    if (strcmp(sysname, "event0") == 0) {
        seen->event0 = true;
    } else if (strcmp(sysname, "event1") == 0) {
        seen->event1 = true;
    }
    return 0;
}

static int check_udev_input_enumeration(void) {
    trace_step("check udev input enumeration");
    struct udev *udev = udev_new();
    if (udev == NULL) {
        fprintf(stderr, "FAIL: udev_new returned NULL\n");
        return 1;
    }

    struct udev_enumerate *enumerate = udev_enumerate_new(udev);
    if (enumerate == NULL) {
        fprintf(stderr, "FAIL: udev_enumerate_new returned NULL\n");
        udev_unref(udev);
        return 1;
    }
    if (udev_enumerate_add_match_subsystem(enumerate, "input") < 0 ||
        udev_enumerate_scan_devices(enumerate) < 0) {
        fprintf(stderr, "FAIL: udev input enumeration failed\n");
        udev_enumerate_unref(enumerate);
        udev_unref(udev);
        return 1;
    }

    struct SeenInput seen = {
        .event0 = false,
        .event1 = false,
    };
    struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
    struct udev_list_entry *entry = NULL;
    udev_list_entry_foreach(entry, devices) {
        const char *syspath = udev_list_entry_get_name(entry);
        struct udev_device *device = udev_device_new_from_syspath(udev, syspath);
        if (device == NULL) {
            fprintf(stderr, "FAIL: udev_device_new_from_syspath %s\n", syspath);
            udev_enumerate_unref(enumerate);
            udev_unref(udev);
            return 1;
        }
        int rc = check_udev_input_device(device, &seen);
        udev_device_unref(device);
        if (rc != 0) {
            udev_enumerate_unref(enumerate);
            udev_unref(udev);
            return rc;
        }
    }

    udev_enumerate_unref(enumerate);
    udev_unref(udev);

    if (!seen.event0 || !seen.event1) {
        fprintf(stderr, "FAIL: udev input enumeration incomplete event0=%d event1=%d\n",
                seen.event0, seen.event1);
        return 1;
    }
    return 0;
}

static int check_udev_devnum_lookup(void) {
    trace_step("check udev devnum lookup");
    struct stat st;
    if (stat("/dev/input/event0", &st) != 0) {
        fprintf(stderr, "FAIL: stat /dev/input/event0: %s\n", strerror(errno));
        return 1;
    }

    struct udev *udev = udev_new();
    if (udev == NULL) {
        fprintf(stderr, "FAIL: udev_new returned NULL for devnum lookup\n");
        return 1;
    }
    struct udev_device *device = udev_device_new_from_devnum(udev, 'c', st.st_rdev);
    if (device == NULL) {
        fprintf(stderr, "FAIL: udev_device_new_from_devnum c %u:%u returned NULL\n",
                major(st.st_rdev), minor(st.st_rdev));
        udev_unref(udev);
        return 1;
    }
    const char *devnode = udev_device_get_devnode(device);
    if (devnode == NULL || strcmp(devnode, "/dev/input/event0") != 0) {
        fprintf(stderr, "FAIL: udev devnum lookup devnode=%s\n",
                devnode == NULL ? "(null)" : devnode);
        udev_device_unref(device);
        udev_unref(udev);
        return 1;
    }
    if (udev_device_get_is_initialized(device) <= 0) {
        fprintf(stderr, "FAIL: udev devnum lookup device is not initialized\n");
        udev_device_unref(device);
        udev_unref(udev);
        return 1;
    }
    udev_device_unref(device);
    udev_unref(udev);
    return 0;
}

static int check_libinput_path_device(const char *path,
                                      enum libinput_device_capability capability,
                                      const char *capability_name) {
    trace_step(path);
    struct libinput *li = libinput_path_create_context(&libinput_iface, NULL);
    if (li == NULL) {
        fprintf(stderr, "FAIL: libinput_path_create_context returned NULL\n");
        return 1;
    }

    struct libinput_device *device = libinput_path_add_device(li, path);
    if (device == NULL) {
        fprintf(stderr, "FAIL: libinput_path_add_device %s returned NULL\n", path);
        libinput_unref(li);
        return 1;
    }

    const char *name = libinput_device_get_name(device);
    if (name == NULL || name[0] == '\0') {
        fprintf(stderr, "FAIL: libinput device has empty name\n");
        libinput_unref(li);
        return 1;
    }
    if (libinput_device_has_capability(device, capability) == 0) {
        fprintf(stderr, "FAIL: libinput path device %s name=%s lacks %s capability\n", path,
                name, capability_name);
        libinput_unref(li);
        return 1;
    }
    printf("libinput path device %s name=%s has %s capability\n", path, name,
           capability_name);

    libinput_path_remove_device(device);
    libinput_unref(li);
    return 0;
}

static int check_libinput_udev_seat(void) {
    trace_step("create udev context for libinput seat");
    struct udev *udev = udev_new();
    if (udev == NULL) {
        fprintf(stderr, "FAIL: udev_new returned NULL for libinput udev backend\n");
        return 1;
    }

    trace_step("create libinput udev context");
    struct libinput *li = libinput_udev_create_context(&libinput_iface, NULL, udev);
    if (li == NULL) {
        fprintf(stderr, "FAIL: libinput_udev_create_context returned NULL\n");
        udev_unref(udev);
        return 1;
    }
    trace_step("assign libinput udev seat0");
    if (libinput_udev_assign_seat(li, "seat0") != 0) {
        fprintf(stderr, "FAIL: libinput_udev_assign_seat seat0 failed\n");
        libinput_unref(li);
        udev_unref(udev);
        return 1;
    }

    trace_step("check libinput udev fd readiness");
    struct pollfd pfd = {
        .fd = libinput_get_fd(li),
        .events = POLLIN,
    };
    int poll_rc = poll(&pfd, 1, 250);
    if (poll_rc < 0) {
        fprintf(stderr, "FAIL: poll libinput udev fd: %s\n", strerror(errno));
        libinput_unref(li);
        udev_unref(udev);
        return 1;
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        fprintf(stderr, "FAIL: libinput udev fd revents=0x%x\n", pfd.revents);
        libinput_unref(li);
        udev_unref(udev);
        return 1;
    }
    if (poll_rc != 0 || pfd.revents != 0) {
        fprintf(stderr, "FAIL: idle libinput udev fd should not be readable poll_rc=%d "
                        "revents=0x%x\n",
                poll_rc, pfd.revents);
        libinput_unref(li);
        udev_unref(udev);
        return 1;
    }
    printf("libinput udev seat assigned; fd=%d poll_rc=%d revents=0x%x\n", pfd.fd, poll_rc,
           pfd.revents);

    trace_step("dispatch libinput udev seat events");
    int dispatch_rc = libinput_dispatch(li);
    if (dispatch_rc != 0) {
        fprintf(stderr, "FAIL: libinput_dispatch returned %d\n", dispatch_rc);
        libinput_unref(li);
        udev_unref(udev);
        return 1;
    }
    int event_count = 0;
    for (;;) {
        struct libinput_event *event = libinput_get_event(li);
        if (event == NULL) {
            break;
        }
        event_count++;
        libinput_event_destroy(event);
    }
    if (event_count < 2) {
        fprintf(stderr, "FAIL: libinput udev dispatch produced %d device events\n", event_count);
        libinput_unref(li);
        udev_unref(udev);
        return 1;
    }
    printf("libinput udev dispatch produced %d device events\n", event_count);

    libinput_unref(li);
    udev_unref(udev);
    return 0;
}

int main(void) {
    signal(SIGALRM, fail_on_timeout);
    alarm(15);

    if (check_udev_input_enumeration() != 0) {
        return 1;
    }
    if (check_udev_devnum_lookup() != 0) {
        return 1;
    }
    if (check_libinput_path_device("/dev/input/event0", LIBINPUT_DEVICE_CAP_POINTER,
                                   "pointer") != 0) {
        return 1;
    }
    if (check_libinput_path_device("/dev/input/event1", LIBINPUT_DEVICE_CAP_KEYBOARD,
                                   "keyboard") != 0) {
        return 1;
    }
    if (check_libinput_udev_seat() != 0) {
        return 1;
    }

    printf("udev input enumeration and libinput path/seat probe tests passed\n");
    printf("All udev input smoke tests passed!\n");
    return 0;
}
