#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static char *read_text_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "FAIL: open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    char *buf = malloc(256);
    if (buf == NULL) {
        fprintf(stderr, "FAIL: malloc for %s\n", path);
        fclose(file);
        return NULL;
    }

    size_t len = fread(buf, 1, 255, file);
    if (ferror(file)) {
        fprintf(stderr, "FAIL: read %s\n", path);
        free(buf);
        fclose(file);
        return NULL;
    }
    buf[len] = '\0';
    fclose(file);
    return buf;
}

static int expect_file_equals(const char *path, const char *expected) {
    char *text = read_text_file(path);
    if (text == NULL) {
        return 1;
    }
    int result = 0;
    if (strcmp(text, expected) != 0) {
        fprintf(stderr, "FAIL: %s contains %s expected %s\n", path, text, expected);
        result = 1;
    }
    free(text);
    return result;
}

static int expect_symlink(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "FAIL: lstat %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISLNK(st.st_mode)) {
        fprintf(stderr, "FAIL: %s is not a symlink mode=%#o\n", path, (unsigned)st.st_mode);
        return 1;
    }
    return 0;
}

static int expect_symlink_resolves(const char *path) {
    if (expect_symlink(path) != 0) {
        return 1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "FAIL: stat symlink target %s: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

static int expect_char_dev_numbers(const char *dev_path, const char *sysfs_path) {
    struct stat st;
    if (stat(dev_path, &st) != 0) {
        fprintf(stderr, "FAIL: stat %s: %s\n", dev_path, strerror(errno));
        return 1;
    }
    if (!S_ISCHR(st.st_mode)) {
        fprintf(stderr, "FAIL: %s is not a character device mode=%#o\n", dev_path,
                (unsigned)st.st_mode);
        return 1;
    }

    char expected[64];
    snprintf(expected, sizeof(expected), "%u:%u\n", major(st.st_rdev), minor(st.st_rdev));
    return expect_file_equals(sysfs_path, expected);
}

static int expect_uevent_devname(const char *path, const char *expected_devname) {
    char *text = read_text_file(path);
    if (text == NULL) {
        return 1;
    }

    char expected[128];
    snprintf(expected, sizeof(expected), "DEVNAME=%s\n", expected_devname);
    int result = strstr(text, expected) == NULL ? 1 : 0;
    if (result != 0) {
        fprintf(stderr, "FAIL: %s missing %s", path, expected);
    }
    free(text);
    return result;
}

static bool dir_contains(const char *path, const char *name) {
    DIR *dir = opendir(path);
    if (dir == NULL) {
        fprintf(stderr, "FAIL: opendir %s: %s\n", path, strerror(errno));
        return false;
    }

    bool found = false;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                fprintf(stderr, "FAIL: readdir %s: %s\n", path, strerror(errno));
            }
            break;
        }
        if (strcmp(entry->d_name, name) == 0) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

static int check_fb_sysfs(void) {
    if (!dir_contains("/sys/class/graphics", "fb0")) {
        fprintf(stderr, "FAIL: /sys/class/graphics missing fb0\n");
        return 1;
    }
    if (expect_char_dev_numbers("/dev/fb0", "/sys/class/graphics/fb0/dev") != 0) {
        return 1;
    }
    if (expect_file_equals("/sys/class/graphics/fb0/name", "fb0\n") != 0) {
        return 1;
    }
    if (expect_uevent_devname("/sys/class/graphics/fb0/uevent", "fb0") != 0) {
        return 1;
    }
    if (expect_symlink("/sys/class/graphics/fb0/device") != 0 ||
        expect_symlink("/sys/class/graphics/fb0/subsystem") != 0) {
        return 1;
    }
    if (expect_symlink_resolves("/sys/dev/char/29:0") != 0) {
        return 1;
    }
    return 0;
}

static int check_input_node(const char *name) {
    char dev_path[128];
    char sys_dev[128];
    char sys_name[128];
    char sys_uevent[128];
    char sys_device[128];
    char sys_subsystem[128];
    char sys_dev_char[128];

    snprintf(dev_path, sizeof(dev_path), "/dev/input/%s", name);
    snprintf(sys_dev, sizeof(sys_dev), "/sys/class/input/%s/dev", name);
    snprintf(sys_name, sizeof(sys_name), "/sys/class/input/%s/name", name);
    snprintf(sys_uevent, sizeof(sys_uevent), "/sys/class/input/%s/uevent", name);
    snprintf(sys_device, sizeof(sys_device), "/sys/class/input/%s/device", name);
    snprintf(sys_subsystem, sizeof(sys_subsystem), "/sys/class/input/%s/subsystem", name);

    if (!dir_contains("/sys/class/input", name)) {
        fprintf(stderr, "FAIL: /sys/class/input missing %s\n", name);
        return 1;
    }
    if (expect_char_dev_numbers(dev_path, sys_dev) != 0) {
        return 1;
    }

    char expected_name[64];
    snprintf(expected_name, sizeof(expected_name), "%s\n", name);
    if (expect_file_equals(sys_name, expected_name) != 0) {
        return 1;
    }
    char expected_devname[64];
    snprintf(expected_devname, sizeof(expected_devname), "input/%s", name);
    if (expect_uevent_devname(sys_uevent, expected_devname) != 0) {
        return 1;
    }
    if (expect_symlink(sys_device) != 0 || expect_symlink(sys_subsystem) != 0) {
        return 1;
    }
    struct stat st;
    if (stat(dev_path, &st) != 0) {
        fprintf(stderr, "FAIL: stat %s: %s\n", dev_path, strerror(errno));
        return 1;
    }
    snprintf(sys_dev_char, sizeof(sys_dev_char), "/sys/dev/char/%u:%u", major(st.st_rdev),
             minor(st.st_rdev));
    if (expect_symlink_resolves(sys_dev_char) != 0) {
        return 1;
    }
    return 0;
}

static int check_input_sysfs(void) {
    if (check_input_node("event0") != 0) {
        return 1;
    }
    if (check_input_node("event1") != 0) {
        return 1;
    }
    if (access("/dev/input/mice", F_OK) == 0 && check_input_node("mice") != 0) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (check_fb_sysfs() != 0) {
        return 1;
    }
    if (check_input_sysfs() != 0) {
        return 1;
    }

    printf("sysfs graphics and input class enumeration tests passed\n");
    printf("All sysfs smoke tests passed!\n");
    return 0;
}
