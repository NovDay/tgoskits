#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define DRM_IOCTL_VERSION 0xc0406400
#define DRM_IOCTL_GET_CAP 0xc010640c
#define DRM_IOCTL_SET_MASTER 0x641e
#define DRM_IOCTL_DROP_MASTER 0x641f
#define DRM_IOCTL_MODE_GETRESOURCES 0xc04064a0

#define DRM_CAP_DUMB_BUFFER 0x1
#define DRM_CAP_TIMESTAMP_MONOTONIC 0x6

struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_mode_card_res {
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

static int expect_text_file_contains(const char *path, const char *needle) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "FAIL: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    char buf[256];
    size_t len = fread(buf, 1, sizeof(buf) - 1, file);
    if (ferror(file)) {
        fprintf(stderr, "FAIL: read %s\n", path);
        fclose(file);
        return 1;
    }
    buf[len] = '\0';
    fclose(file);
    if (strstr(buf, needle) == NULL) {
        fprintf(stderr, "FAIL: %s missing %s, got %s\n", path, needle, buf);
        return 1;
    }
    return 0;
}

static int expect_char_device(const char *path, unsigned expected_major, unsigned expected_minor) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "FAIL: stat %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISCHR(st.st_mode)) {
        fprintf(stderr, "FAIL: %s is not char device mode=%#o\n", path, (unsigned)st.st_mode);
        return 1;
    }
    if (major(st.st_rdev) != expected_major || minor(st.st_rdev) != expected_minor) {
        fprintf(stderr, "FAIL: %s rdev=%u:%u expected=%u:%u\n", path, major(st.st_rdev),
                minor(st.st_rdev), expected_major, expected_minor);
        return 1;
    }
    return 0;
}

static int expect_drm_sysfs(void) {
    if (expect_char_device("/dev/dri/card0", 226, 0) != 0) {
        return 1;
    }
    if (expect_text_file_contains("/sys/class/drm/card0/dev", "226:0\n") != 0) {
        return 1;
    }
    if (expect_text_file_contains("/sys/class/drm/card0/name", "card0\n") != 0) {
        return 1;
    }
    if (expect_text_file_contains("/sys/class/drm/card0/uevent", "DEVNAME=dri/card0\n") != 0) {
        return 1;
    }
    if (expect_text_file_contains("/run/udev/data/c226:0", "E:DEVNAME=/dev/dri/card0\n") != 0) {
        return 1;
    }
    if (expect_text_file_contains("/run/udev/data/c226:0", "E:ID_PATH=platform-starry-drm\n") !=
        0) {
        return 1;
    }
    return 0;
}

static int expect_version(int fd) {
    char name[32] = {0};
    char date[32] = {0};
    char desc[64] = {0};
    struct drm_version version = {
        .name_len = sizeof(name),
        .name = name,
        .date_len = sizeof(date),
        .date = date,
        .desc_len = sizeof(desc),
        .desc = desc,
    };
    if (ioctl(fd, DRM_IOCTL_VERSION, &version) != 0) {
        fprintf(stderr, "FAIL: DRM_IOCTL_VERSION: %s\n", strerror(errno));
        return 1;
    }
    if (version.version_major != 0 || version.version_minor != 1 ||
        version.version_patchlevel != 0 || strcmp(name, "starrydrm") != 0 ||
        strcmp(date, "20260509") != 0 ||
        strcmp(desc, "StarryOS framebuffer-backed DRM stub") != 0) {
        fprintf(stderr, "FAIL: version major=%d minor=%d patch=%d name=%s date=%s desc=%s\n",
                version.version_major, version.version_minor, version.version_patchlevel, name,
                date, desc);
        return 1;
    }

    struct drm_version lengths = {0};
    if (ioctl(fd, DRM_IOCTL_VERSION, &lengths) != 0) {
        fprintf(stderr, "FAIL: DRM_IOCTL_VERSION length probe: %s\n", strerror(errno));
        return 1;
    }
    if (lengths.name_len != strlen("starrydrm") || lengths.date_len != strlen("20260509") ||
        lengths.desc_len != strlen("StarryOS framebuffer-backed DRM stub")) {
        fprintf(stderr, "FAIL: version lengths name=%zu date=%zu desc=%zu\n", lengths.name_len,
                lengths.date_len, lengths.desc_len);
        return 1;
    }
    return 0;
}

static int expect_cap(int fd, uint64_t capability, uint64_t expected) {
    struct drm_get_cap cap = {
        .capability = capability,
        .value = UINT64_MAX,
    };
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) != 0) {
        fprintf(stderr, "FAIL: DRM_IOCTL_GET_CAP %#llx: %s\n",
                (unsigned long long)capability, strerror(errno));
        return 1;
    }
    if (cap.value != expected) {
        fprintf(stderr, "FAIL: DRM cap %#llx value=%#llx expected=%#llx\n",
                (unsigned long long)capability, (unsigned long long)cap.value,
                (unsigned long long)expected);
        return 1;
    }
    return 0;
}

int main(void) {
    if (expect_drm_sysfs() != 0) {
        return 1;
    }

    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "FAIL: open /dev/dri/card0: %s\n", strerror(errno));
        return 1;
    }

    if (expect_version(fd) != 0 || expect_cap(fd, DRM_CAP_DUMB_BUFFER, 0) != 0 ||
        expect_cap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, 1) != 0 || ioctl(fd, DRM_IOCTL_SET_MASTER) != 0 ||
        ioctl(fd, DRM_IOCTL_DROP_MASTER) != 0) {
        close(fd);
        return 1;
    }

    struct drm_mode_card_res res = {0};
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 || errno != ENOSYS) {
        fprintf(stderr, "FAIL: MODE_GETRESOURCES ret/errno mismatch errno=%s\n", strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);
    printf("DRM card0 device, sysfs, udev and basic ioctl probes passed\n");
    printf("All DRM smoke tests passed!\n");
    return 0;
}
