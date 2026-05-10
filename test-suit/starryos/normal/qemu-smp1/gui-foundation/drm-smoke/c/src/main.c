#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define DRM_IOCTL_VERSION 0xc0406400
#define DRM_IOCTL_GET_CAP 0xc010640c
#define DRM_IOCTL_SET_CLIENT_CAP 0x4010640d
#define DRM_IOCTL_SET_MASTER 0x641e
#define DRM_IOCTL_DROP_MASTER 0x641f
#define DRM_IOCTL_MODE_GETRESOURCES 0xc04064a0
#define DRM_IOCTL_MODE_GETCRTC 0xc06864a1
#define DRM_IOCTL_MODE_SETCRTC 0xc06864a2
#define DRM_IOCTL_MODE_GETENCODER 0xc01464a6
#define DRM_IOCTL_MODE_GETCONNECTOR 0xc05064a7
#define DRM_IOCTL_MODE_GETPROPERTY 0xc04064aa
#define DRM_IOCTL_MODE_PAGE_FLIP 0xc01864b0
#define DRM_IOCTL_MODE_GETPLANERESOURCES 0xc01064b5
#define DRM_IOCTL_MODE_GETPLANE 0xc02064b6
#define DRM_IOCTL_MODE_ADDFB2 0xc06864b8
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES 0xc02064b9
#define DRM_IOCTL_MODE_CREATE_DUMB 0xc02064b2
#define DRM_IOCTL_MODE_MAP_DUMB 0xc01064b3
#define DRM_IOCTL_MODE_DESTROY_DUMB 0xc00464b4
#define FBIOGET_FSCREENINFO 0x4602

#define DRM_CAP_DUMB_BUFFER 0x1
#define DRM_CAP_TIMESTAMP_MONOTONIC 0x6
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_MODE_PROP_IMMUTABLE (1U << 2)
#define DRM_MODE_PROP_ENUM (1U << 3)
#define DRM_MODE_TYPE_PREFERRED (1U << 3)
#define DRM_MODE_TYPE_DRIVER (1U << 6)
#define DRM_MODE_CONNECTOR_VIRTUAL 15
#define DRM_MODE_CONNECTED 1
#define DRM_MODE_SUBPIXEL_UNKNOWN 1
#define DRM_MODE_ENCODER_VIRTUAL 5
#define DRM_MODE_OBJECT_CRTC 0xccccccccU
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0U
#define DRM_MODE_OBJECT_ENCODER 0xe0e0e0e0U
#define DRM_MODE_OBJECT_PLANE 0xeeeeeeeeU
#define DRM_MODE_PAGE_FLIP_EVENT 0x1
#define DRM_EVENT_FLIP_COMPLETE 0x02
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_FORMAT_XRGB8888 0x34325258U
#define DRM_FORMAT_ARGB8888 0x34325241U

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

struct drm_set_client_cap {
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

struct drm_mode_modeinfo {
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

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

struct drm_mode_get_property {
    uint64_t values_ptr;
    uint64_t enum_blob_ptr;
    uint32_t prop_id;
    uint32_t flags;
    char name[32];
    uint32_t count_values;
    uint32_t count_enum_blobs;
};

struct drm_mode_property_enum {
    uint64_t value;
    char name[32];
};

struct drm_mode_get_plane_res {
    uint64_t plane_id_ptr;
    uint32_t count_planes;
};

struct drm_mode_get_plane {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t count_format_types;
    uint64_t format_type_ptr;
};

struct drm_mode_obj_get_properties {
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_props;
    uint32_t obj_id;
    uint32_t obj_type;
};

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};

struct drm_mode_crtc_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
};

struct drm_event {
    uint32_t type;
    uint32_t length;
};

struct drm_event_vblank {
    struct drm_event base;
    uint64_t user_data;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t sequence;
    uint32_t crtc_id;
};

struct drm_resource_ids {
    uint32_t crtc_id;
    uint32_t connector_id;
    uint32_t encoder_id;
    uint32_t plane_id;
    uint32_t max_width;
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

static int expect_client_cap(int fd, uint64_t capability, uint64_t value) {
    struct drm_set_client_cap cap = {
        .capability = capability,
        .value = value,
    };
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) != 0) {
        fprintf(stderr, "FAIL: DRM_IOCTL_SET_CLIENT_CAP %#llx=%#llx: %s\n",
                (unsigned long long)capability, (unsigned long long)value, strerror(errno));
        return 1;
    }
    return 0;
}

static int expect_getresources(int fd, struct drm_resource_ids *ids) {
    struct drm_mode_card_res res = {0};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        fprintf(stderr, "FAIL: MODE_GETRESOURCES count probe: %s\n", strerror(errno));
        return 1;
    }
    if (res.count_fbs != 0 || res.count_crtcs != 1 || res.count_connectors != 1 ||
        res.count_encoders != 1 || res.max_width == 0 || res.max_height == 0 ||
        res.min_width != 0 || res.min_height != 0) {
        fprintf(stderr,
                "FAIL: resources counts fbs=%u crtcs=%u connectors=%u encoders=%u "
                "min=%ux%u max=%ux%u\n",
                res.count_fbs, res.count_crtcs, res.count_connectors, res.count_encoders,
                res.min_width, res.min_height, res.max_width, res.max_height);
        return 1;
    }

    ids->crtc_id = 0;
    ids->connector_id = 0;
    ids->encoder_id = 0;
    ids->max_width = res.max_width;
    ids->max_height = res.max_height;
    res.crtc_id_ptr = (uintptr_t)&ids->crtc_id;
    res.connector_id_ptr = (uintptr_t)&ids->connector_id;
    res.encoder_id_ptr = (uintptr_t)&ids->encoder_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        fprintf(stderr, "FAIL: MODE_GETRESOURCES id probe: %s\n", strerror(errno));
        return 1;
    }
    if (ids->crtc_id == 0 || ids->connector_id == 0 || ids->encoder_id == 0 ||
        ids->crtc_id == ids->connector_id || ids->crtc_id == ids->encoder_id ||
        ids->connector_id == ids->encoder_id) {
        fprintf(stderr, "FAIL: resource ids crtc=%u connector=%u encoder=%u\n", ids->crtc_id,
                ids->connector_id, ids->encoder_id);
        return 1;
    }

    uint32_t crtc_id = 0;
    uint32_t connector_id = 0;
    uint32_t encoder_id = 0;
    res.crtc_id_ptr = (uintptr_t)&crtc_id;
    res.connector_id_ptr = (uintptr_t)&connector_id;
    res.encoder_id_ptr = (uintptr_t)&encoder_id;
    res.count_crtcs = 0;
    res.count_connectors = 0;
    res.count_encoders = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        fprintf(stderr, "FAIL: MODE_GETRESOURCES zero-count probe: %s\n", strerror(errno));
        return 1;
    }
    if (crtc_id != 0 || connector_id != 0 || encoder_id != 0 || res.count_crtcs != 1 ||
        res.count_connectors != 1 || res.count_encoders != 1) {
        fprintf(stderr,
                "FAIL: zero-count resources ids crtc=%u connector=%u encoder=%u "
                "counts=%u/%u/%u\n",
                crtc_id, connector_id, encoder_id, res.count_crtcs, res.count_connectors,
                res.count_encoders);
        return 1;
    }
    return 0;
}

static int expect_kms_topology(int fd, const struct drm_resource_ids *ids) {
    struct drm_mode_get_connector connector = {
        .connector_id = ids->connector_id,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) != 0) {
        fprintf(stderr, "FAIL: MODE_GETCONNECTOR count probe: %s\n", strerror(errno));
        return 1;
    }
    if (connector.count_modes != 1 || connector.count_props != 0 || connector.count_encoders != 1 ||
        connector.encoder_id != ids->encoder_id ||
        connector.connector_type != DRM_MODE_CONNECTOR_VIRTUAL ||
        connector.connector_type_id != 1 || connector.connection != DRM_MODE_CONNECTED ||
        connector.subpixel != DRM_MODE_SUBPIXEL_UNKNOWN) {
        fprintf(stderr,
                "FAIL: connector counts=%u/%u/%u encoder=%u type=%u type_id=%u "
                "connection=%u subpixel=%u\n",
                connector.count_modes, connector.count_props, connector.count_encoders,
                connector.encoder_id, connector.connector_type, connector.connector_type_id,
                connector.connection, connector.subpixel);
        return 1;
    }

    uint32_t encoder_id = 0;
    struct drm_mode_modeinfo mode = {0};
    connector.encoders_ptr = (uintptr_t)&encoder_id;
    connector.modes_ptr = (uintptr_t)&mode;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) != 0) {
        fprintf(stderr, "FAIL: MODE_GETCONNECTOR payload probe: %s\n", strerror(errno));
        return 1;
    }
    if (encoder_id != ids->encoder_id || connector.count_modes != 1 ||
        connector.count_encoders != 1 || mode.hdisplay != ids->max_width ||
        mode.vdisplay != ids->max_height || mode.vrefresh != 60 ||
        (mode.type & (DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER)) !=
            (DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER) ||
        mode.name[0] == '\0') {
        fprintf(stderr,
                "FAIL: connector payload encoder=%u mode=%ux%u@%u type=%#x name=%s "
                "expected=%u/%ux%u\n",
                encoder_id, mode.hdisplay, mode.vdisplay, mode.vrefresh, mode.type, mode.name,
                ids->encoder_id, ids->max_width, ids->max_height);
        return 1;
    }

    struct drm_mode_get_encoder encoder = {
        .encoder_id = ids->encoder_id,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) != 0) {
        fprintf(stderr, "FAIL: MODE_GETENCODER: %s\n", strerror(errno));
        return 1;
    }
    if (encoder.encoder_type != DRM_MODE_ENCODER_VIRTUAL || encoder.crtc_id != ids->crtc_id ||
        encoder.possible_crtcs != 1 || encoder.possible_clones != 0) {
        fprintf(stderr,
                "FAIL: encoder type=%u crtc=%u possible_crtcs=%#x clones=%#x expected_crtc=%u\n",
                encoder.encoder_type, encoder.crtc_id, encoder.possible_crtcs,
                encoder.possible_clones, ids->crtc_id);
        return 1;
    }

    struct drm_mode_crtc crtc = {
        .crtc_id = ids->crtc_id,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) != 0) {
        fprintf(stderr, "FAIL: MODE_GETCRTC: %s\n", strerror(errno));
        return 1;
    }
    if (crtc.fb_id != 0 || crtc.x != 0 || crtc.y != 0 || crtc.mode_valid != 1 ||
        crtc.mode.hdisplay != ids->max_width || crtc.mode.vdisplay != ids->max_height ||
        crtc.mode.vrefresh != 60 || crtc.mode.name[0] == '\0') {
        fprintf(stderr,
                "FAIL: crtc fb=%u pos=%u,%u valid=%u mode=%ux%u@%u name=%s "
                "expected=%ux%u\n",
                crtc.fb_id, crtc.x, crtc.y, crtc.mode_valid, crtc.mode.hdisplay,
                crtc.mode.vdisplay, crtc.mode.vrefresh, crtc.mode.name, ids->max_width,
                ids->max_height);
        return 1;
    }
    return 0;
}

static int expect_object_has_no_properties(int fd, uint32_t obj_id, uint32_t obj_type) {
    uint32_t prop_id = UINT32_MAX;
    uint64_t prop_value = UINT64_MAX;
    struct drm_mode_obj_get_properties props = {
        .props_ptr = (uintptr_t)&prop_id,
        .prop_values_ptr = (uintptr_t)&prop_value,
        .count_props = 1,
        .obj_id = obj_id,
        .obj_type = obj_type,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) != 0) {
        fprintf(stderr, "FAIL: MODE_OBJ_GETPROPERTIES obj=%u type=%#x: %s\n", obj_id,
                obj_type, strerror(errno));
        return 1;
    }
    if (props.count_props != 0 || prop_id != UINT32_MAX || prop_value != UINT64_MAX) {
        fprintf(stderr,
                "FAIL: object properties obj=%u type=%#x count=%u prop=%u value=%#llx\n",
                obj_id, obj_type, props.count_props, prop_id, (unsigned long long)prop_value);
        return 1;
    }
    return 0;
}

static int expect_plane_type_property(int fd, uint32_t plane_id) {
    uint32_t prop_id = UINT32_MAX;
    uint64_t prop_value = UINT64_MAX;
    struct drm_mode_obj_get_properties props = {
        .props_ptr = (uintptr_t)&prop_id,
        .prop_values_ptr = (uintptr_t)&prop_value,
        .count_props = 1,
        .obj_id = plane_id,
        .obj_type = DRM_MODE_OBJECT_PLANE,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) != 0) {
        fprintf(stderr, "FAIL: MODE_OBJ_GETPROPERTIES plane=%u: %s\n", plane_id,
                strerror(errno));
        return 1;
    }
    if (props.count_props != 1 || prop_id == UINT32_MAX ||
        prop_value != DRM_PLANE_TYPE_PRIMARY) {
        fprintf(stderr, "FAIL: plane props count=%u prop=%u value=%#llx\n",
                props.count_props, prop_id, (unsigned long long)prop_value);
        return 1;
    }

    struct drm_mode_get_property property = {
        .prop_id = prop_id,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &property) != 0) {
        fprintf(stderr, "FAIL: MODE_GETPROPERTY plane type count probe: %s\n",
                strerror(errno));
        return 1;
    }
    if (strcmp(property.name, "type") != 0 ||
        property.flags != (DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE) ||
        property.count_values != 3 || property.count_enum_blobs != 3) {
        fprintf(stderr,
                "FAIL: plane type property name=%s flags=%#x values=%u enums=%u\n",
                property.name, property.flags, property.count_values,
                property.count_enum_blobs);
        return 1;
    }

    uint64_t values[3] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    struct drm_mode_property_enum enums[3] = {0};
    property.values_ptr = (uintptr_t)values;
    property.enum_blob_ptr = (uintptr_t)enums;
    property.count_values = 3;
    property.count_enum_blobs = 3;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &property) != 0) {
        fprintf(stderr, "FAIL: MODE_GETPROPERTY plane type values probe: %s\n",
                strerror(errno));
        return 1;
    }
    if (values[0] != 0 || values[1] != DRM_PLANE_TYPE_PRIMARY || values[2] != 2 ||
        enums[0].value != 0 ||
        strcmp(enums[0].name, "Overlay") != 0 ||
        enums[1].value != DRM_PLANE_TYPE_PRIMARY ||
        strcmp(enums[1].name, "Primary") != 0 || enums[2].value != 2 ||
        strcmp(enums[2].name, "Cursor") != 0) {
        fprintf(stderr,
                "FAIL: plane type values=%llu/%llu/%llu enums=%llu:%s,%llu:%s,%llu:%s\n",
                (unsigned long long)values[0], (unsigned long long)values[1],
                (unsigned long long)values[2],
                (unsigned long long)enums[0].value, enums[0].name,
                (unsigned long long)enums[1].value, enums[1].name,
                (unsigned long long)enums[2].value, enums[2].name);
        return 1;
    }
    return 0;
}

static int expect_plane_and_properties(int fd, struct drm_resource_ids *ids) {
    struct drm_mode_get_plane_res plane_res = {0};
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_res) != 0) {
        fprintf(stderr, "FAIL: MODE_GETPLANERESOURCES count probe: %s\n", strerror(errno));
        return 1;
    }
    if (plane_res.count_planes != 1) {
        fprintf(stderr, "FAIL: plane resource count=%u\n", plane_res.count_planes);
        return 1;
    }

    ids->plane_id = 0;
    plane_res.plane_id_ptr = (uintptr_t)&ids->plane_id;
    plane_res.count_planes = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_res) != 0) {
        fprintf(stderr, "FAIL: MODE_GETPLANERESOURCES id probe: %s\n", strerror(errno));
        return 1;
    }
    if (ids->plane_id == 0 || plane_res.count_planes != 1) {
        fprintf(stderr, "FAIL: plane id=%u count=%u\n", ids->plane_id, plane_res.count_planes);
        return 1;
    }

    uint32_t zero_count_plane_id = UINT32_MAX;
    plane_res.plane_id_ptr = (uintptr_t)&zero_count_plane_id;
    plane_res.count_planes = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_res) != 0) {
        fprintf(stderr, "FAIL: MODE_GETPLANERESOURCES zero-count probe: %s\n",
                strerror(errno));
        return 1;
    }
    if (zero_count_plane_id != UINT32_MAX || plane_res.count_planes != 1) {
        fprintf(stderr, "FAIL: zero-count plane id=%u count=%u\n", zero_count_plane_id,
                plane_res.count_planes);
        return 1;
    }

    struct drm_mode_get_plane plane = {
        .plane_id = ids->plane_id,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) != 0) {
        fprintf(stderr, "FAIL: MODE_GETPLANE count probe: %s\n", strerror(errno));
        return 1;
    }
    if (plane.crtc_id != ids->crtc_id || plane.possible_crtcs != 1 || plane.gamma_size != 0 ||
        plane.count_format_types != 2) {
        fprintf(stderr, "FAIL: plane state crtc=%u possible=%#x gamma=%u formats=%u\n",
                plane.crtc_id, plane.possible_crtcs, plane.gamma_size,
                plane.count_format_types);
        return 1;
    }

    uint32_t formats[2] = {0, 0};
    plane.format_type_ptr = (uintptr_t)formats;
    plane.count_format_types = 2;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) != 0) {
        fprintf(stderr, "FAIL: MODE_GETPLANE format probe: %s\n", strerror(errno));
        return 1;
    }
    if (formats[0] != DRM_FORMAT_XRGB8888 || formats[1] != DRM_FORMAT_ARGB8888 ||
        plane.count_format_types != 2) {
        fprintf(stderr, "FAIL: plane formats=%#x/%#x count=%u\n", formats[0], formats[1],
                plane.count_format_types);
        return 1;
    }

    if (expect_object_has_no_properties(fd, ids->crtc_id, DRM_MODE_OBJECT_CRTC) != 0 ||
        expect_object_has_no_properties(fd, ids->connector_id, DRM_MODE_OBJECT_CONNECTOR) != 0 ||
        expect_object_has_no_properties(fd, ids->encoder_id, DRM_MODE_OBJECT_ENCODER) != 0 ||
        expect_plane_type_property(fd, ids->plane_id) != 0) {
        return 1;
    }

    struct drm_mode_get_property property = {
        .prop_id = 1,
    };
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &property) == 0 || errno != EINVAL) {
        fprintf(stderr, "FAIL: MODE_GETPROPERTY unknown property errno=%s\n", strerror(errno));
        return 1;
    }

    printf("KMS plane resources and primary type property probes passed\n");
    return 0;
}

static int expect_kms_dumb_scanout(int fd, const struct drm_resource_ids *ids) {
    const uint32_t width = ids->max_width >= 8 ? 8 : ids->max_width;
    const uint32_t height = ids->max_height >= 8 ? 8 : ids->max_height;
    if (width == 0 || height == 0) {
        fprintf(stderr, "FAIL: invalid KMS dimensions %ux%u\n", width, height);
        return 1;
    }

    struct drm_mode_create_dumb create = {
        .height = height,
        .width = width,
        .bpp = 32,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        fprintf(stderr, "FAIL: MODE_CREATE_DUMB: %s\n", strerror(errno));
        return 1;
    }
    if (create.handle == 0 || create.pitch < width * 4 || create.size < create.pitch * height) {
        fprintf(stderr, "FAIL: dumb create handle=%u pitch=%u size=%llu geometry=%ux%u\n",
                create.handle, create.pitch, (unsigned long long)create.size, width, height);
        return 1;
    }

    struct drm_mode_map_dumb map = {
        .handle = create.handle,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        fprintf(stderr, "FAIL: MODE_MAP_DUMB: %s\n", strerror(errno));
        return 1;
    }
    if (map.offset == 0 || (map.offset & 0xfff) != 0) {
        fprintf(stderr, "FAIL: dumb map offset=%#llx\n", (unsigned long long)map.offset);
        return 1;
    }

    uint8_t *pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    if (pixels == MAP_FAILED) {
        fprintf(stderr, "FAIL: mmap dumb buffer: %s\n", strerror(errno));
        return 1;
    }
    const uint32_t red = 0x00ff0000;
    const uint32_t green = 0x0000ff00;
    const uint32_t blue = 0x000000ff;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t pixel = x == 0 && y == 0 ? red : (x == 1 && y == 0 ? green : blue);
            memcpy(pixels + (size_t)y * create.pitch + (size_t)x * 4, &pixel, sizeof(pixel));
        }
    }

    struct drm_mode_fb_cmd2 fb = {
        .width = width,
        .height = height,
        .pixel_format = DRM_FORMAT_XRGB8888,
        .handles = {create.handle, 0, 0, 0},
        .pitches = {create.pitch, 0, 0, 0},
    };
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) != 0) {
        fprintf(stderr, "FAIL: MODE_ADDFB2: %s\n", strerror(errno));
        munmap(pixels, create.size);
        return 1;
    }
    if (fb.fb_id == 0) {
        fprintf(stderr, "FAIL: MODE_ADDFB2 returned fb_id=0\n");
        munmap(pixels, create.size);
        return 1;
    }

    uint32_t connector_id = ids->connector_id;
    struct drm_mode_crtc crtc = {
        .set_connectors_ptr = (uintptr_t)&connector_id,
        .count_connectors = 1,
        .crtc_id = ids->crtc_id,
        .fb_id = fb.fb_id,
        .mode_valid = 1,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) != 0) {
        fprintf(stderr, "FAIL: MODE_GETCRTC before SETCRTC: %s\n", strerror(errno));
        munmap(pixels, create.size);
        return 1;
    }
    crtc.set_connectors_ptr = (uintptr_t)&connector_id;
    crtc.count_connectors = 1;
    crtc.fb_id = fb.fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) != 0) {
        fprintf(stderr, "FAIL: MODE_SETCRTC: %s\n", strerror(errno));
        munmap(pixels, create.size);
        return 1;
    }

    struct drm_mode_crtc current = {
        .crtc_id = ids->crtc_id,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &current) != 0 || current.fb_id != fb.fb_id) {
        fprintf(stderr, "FAIL: MODE_GETCRTC after SETCRTC ret fb=%u expected=%u errno=%s\n",
                current.fb_id, fb.fb_id, strerror(errno));
        munmap(pixels, create.size);
        return 1;
    }

    int fb_fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (fb_fd < 0) {
        fprintf(stderr, "FAIL: open /dev/fb0 for KMS readback: %s\n", strerror(errno));
        munmap(pixels, create.size);
        return 1;
    }
    struct fb_fix_screeninfo fix = {0};
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        fprintf(stderr, "FAIL: FBIOGET_FSCREENINFO for KMS readback: %s\n", strerror(errno));
        close(fb_fd);
        munmap(pixels, create.size);
        return 1;
    }
    uint32_t readback[2] = {0};
    ssize_t n = pread(fb_fd, readback, sizeof(readback), 0);
    close(fb_fd);
    if (n != (ssize_t)sizeof(readback) || readback[0] != red || readback[1] != green ||
        fix.line_length == 0) {
        fprintf(stderr,
                "FAIL: KMS scanout readback n=%zd pixels=%#x/%#x expected=%#x/%#x line=%u\n",
                n, readback[0], readback[1], red, green, fix.line_length);
        munmap(pixels, create.size);
        return 1;
    }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t pixel = x == 0 && y == 0 ? green : (x == 1 && y == 0 ? red : blue);
            memcpy(pixels + (size_t)y * create.pitch + (size_t)x * 4, &pixel, sizeof(pixel));
        }
    }

    const uint64_t user_data = 0x1122334455667788ULL;
    struct drm_mode_crtc_page_flip flip = {
        .crtc_id = ids->crtc_id,
        .fb_id = fb.fb_id,
        .flags = DRM_MODE_PAGE_FLIP_EVENT,
        .user_data = user_data,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) != 0) {
        fprintf(stderr, "FAIL: MODE_PAGE_FLIP: %s\n", strerror(errno));
        munmap(pixels, create.size);
        return 1;
    }

    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN,
    };
    if (poll(&pfd, 1, 0) != 1 || (pfd.revents & POLLIN) == 0) {
        fprintf(stderr, "FAIL: poll page flip event revents=%#x errno=%s\n", pfd.revents,
                strerror(errno));
        munmap(pixels, create.size);
        return 1;
    }

    struct drm_event_vblank event = {0};
    ssize_t event_n = read(fd, &event, sizeof(event));
    if (event_n != (ssize_t)sizeof(event) || event.base.type != DRM_EVENT_FLIP_COMPLETE ||
        event.base.length != sizeof(event) || event.user_data != user_data ||
        event.sequence == 0 || event.crtc_id != ids->crtc_id) {
        fprintf(stderr,
                "FAIL: page flip event n=%zd type=%u len=%u data=%#llx seq=%u crtc=%u "
                "expected_data=%#llx expected_crtc=%u\n",
                event_n, event.base.type, event.base.length,
                (unsigned long long)event.user_data, event.sequence, event.crtc_id,
                (unsigned long long)user_data, ids->crtc_id);
        munmap(pixels, create.size);
        return 1;
    }

    pfd.revents = 0;
    if (poll(&pfd, 1, 0) != 0 || pfd.revents != 0) {
        fprintf(stderr, "FAIL: page flip event queue drained revents=%#x\n", pfd.revents);
        munmap(pixels, create.size);
        return 1;
    }

    fb_fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (fb_fd < 0) {
        fprintf(stderr, "FAIL: reopen /dev/fb0 after page flip: %s\n", strerror(errno));
        munmap(pixels, create.size);
        return 1;
    }
    memset(readback, 0, sizeof(readback));
    n = pread(fb_fd, readback, sizeof(readback), 0);
    close(fb_fd);
    if (n != (ssize_t)sizeof(readback) || readback[0] != green || readback[1] != red) {
        fprintf(stderr,
                "FAIL: page flip readback n=%zd pixels=%#x/%#x expected=%#x/%#x\n", n,
                readback[0], readback[1], green, red);
        munmap(pixels, create.size);
        return 1;
    }

    if (munmap(pixels, create.size) != 0) {
        fprintf(stderr, "FAIL: munmap dumb buffer: %s\n", strerror(errno));
        return 1;
    }
    struct drm_mode_destroy_dumb destroy = {
        .handle = create.handle,
    };
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) == 0 || errno != EINVAL) {
        fprintf(stderr, "FAIL: DESTROY_DUMB active buffer errno=%s\n", strerror(errno));
        return 1;
    }

    printf("KMS dumb buffer mmap, ADDFB2, SETCRTC and page flip event tests passed\n");
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

    struct drm_resource_ids ids = {0};
    if (expect_version(fd) != 0 || expect_cap(fd, DRM_CAP_DUMB_BUFFER, 1) != 0 ||
        expect_cap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, 1) != 0 ||
        expect_client_cap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 ||
        ioctl(fd, DRM_IOCTL_SET_MASTER) != 0 ||
        ioctl(fd, DRM_IOCTL_DROP_MASTER) != 0 || expect_getresources(fd, &ids) != 0 ||
        expect_kms_topology(fd, &ids) != 0 || expect_plane_and_properties(fd, &ids) != 0 ||
        expect_kms_dumb_scanout(fd, &ids) != 0) {
        close(fd);
        return 1;
    }

    close(fd);
    printf("DRM card0 device, sysfs, udev and KMS scanout probes passed\n");
    printf("All DRM smoke tests passed!\n");
    return 0;
}
