#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EV_VERSION 0x010001
#define EV_KEY 0x01
#define EVIOCGVERSION 0x80044501
#define EVIOCGID 0x80084502
#define EVIOCGBIT(ev, len) (0x80004520 + ((len) << 16) + (ev))
#define EVIOCGNAME(len) (0x80004506 + ((len) << 16))

struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

static bool has_token(const char *name, const char *token) {
    return strstr(name, token) != NULL;
}

static int test_device(const char *path, const char *expected_token, char *name,
                       size_t name_size) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "FAIL: open %s: %s\n", path, strerror(errno));
        return 1;
    }

    int version = 0;
    if (ioctl(fd, EVIOCGVERSION, &version) != 0) {
        fprintf(stderr, "FAIL: EVIOCGVERSION on %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    if (version != EV_VERSION) {
        fprintf(stderr, "FAIL: unexpected evdev version %#x on %s\n", version, path);
        close(fd);
        return 1;
    }

    struct input_id id = {0};
    if (ioctl(fd, EVIOCGID, &id) != 0) {
        fprintf(stderr, "FAIL: EVIOCGID on %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }

    if (name_size == 0) {
        fprintf(stderr, "FAIL: invalid name buffer for %s\n", path);
        close(fd);
        return 1;
    }
    memset(name, 0, name_size);
    int name_len = ioctl(fd, EVIOCGNAME(name_size - 1), name);
    if (name_len <= 0) {
        fprintf(stderr, "FAIL: EVIOCGNAME on %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    if (expected_token != NULL && !has_token(name, expected_token)) {
        fprintf(stderr, "FAIL: %s name=%s does not contain %s\n", path, name, expected_token);
        close(fd);
        return 1;
    }

    uint8_t ev_bits[8] = {0};
    int ev_len = ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);
    if (ev_len <= 0) {
        fprintf(stderr, "FAIL: EVIOCGBIT(0) on %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    if ((ev_bits[EV_KEY / 8] & (1u << (EV_KEY % 8))) == 0) {
        fprintf(stderr, "FAIL: %s does not advertise EV_KEY\n", path);
        close(fd);
        return 1;
    }

    printf("evdev %s name=%s id=%04x:%04x:%04x:%04x version=%#x\n",
           path, name, id.bustype, id.vendor, id.product, id.version, version);
    close(fd);
    return 0;
}

int main(void) {
    if (access("/dev/input", F_OK) != 0) {
        fprintf(stderr, "FAIL: /dev/input is missing: %s\n", strerror(errno));
        return 1;
    }

    char event0_name[128];
    char event1_name[128];
    char mice_name[128];
    if (test_device("/dev/input/event0", NULL, event0_name, sizeof(event0_name)) != 0) {
        return 1;
    }
    if (test_device("/dev/input/event1", NULL, event1_name, sizeof(event1_name)) != 0) {
        return 1;
    }
    if (!((has_token(event0_name, "Mouse") && has_token(event1_name, "Keyboard")) ||
          (has_token(event0_name, "Keyboard") && has_token(event1_name, "Mouse")))) {
        fprintf(stderr, "FAIL: expected one mouse and one keyboard, got event0=%s event1=%s\n",
                event0_name, event1_name);
        return 1;
    }
    if (test_device("/dev/input/mice", "Mouse", mice_name, sizeof(mice_name)) != 0) {
        return 1;
    }

    printf("evdev input directory contains mouse and keyboard event nodes\n");
    printf("evdev mice compatibility node name=%s\n", mice_name);

    printf("All evdev smoke tests passed!\n");
    return 0;
}
