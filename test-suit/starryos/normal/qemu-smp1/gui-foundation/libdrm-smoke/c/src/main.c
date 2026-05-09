#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int fail_errno(const char *what) {
    fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno));
    return 1;
}

static int fail_msg(const char *what) {
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

static bool mode_matches_crtc(const drmModeModeInfo *left, const drmModeModeInfo *right) {
    return left->hdisplay == right->hdisplay && left->vdisplay == right->vdisplay &&
           left->vrefresh == right->vrefresh && strncmp(left->name, right->name, sizeof(left->name)) == 0;
}

static bool plane_supports_format(const drmModePlane *plane, uint32_t format) {
    for (uint32_t i = 0; i < plane->count_formats; i++) {
        if (plane->formats[i] == format) {
            return true;
        }
    }
    return false;
}

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return fail_errno("open /dev/dri/card0");
    }

    drmVersionPtr version = drmGetVersion(fd);
    if (version == NULL) {
        close(fd);
        return fail_msg("drmGetVersion returned NULL");
    }
    if (version->name == NULL || strcmp(version->name, "starrydrm") != 0) {
        fprintf(stderr, "FAIL: unexpected DRM driver name: %s\n",
                version->name != NULL ? version->name : "(null)");
        drmFreeVersion(version);
        close(fd);
        return 1;
    }
    printf("DRM driver: %s %d.%d.%d\n", version->name, version->version_major,
           version->version_minor, version->version_patchlevel);
    drmFreeVersion(version);

    uint64_t dumb_cap = 0;
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &dumb_cap) != 0) {
        close(fd);
        return fail_errno("drmGetCap(DRM_CAP_DUMB_BUFFER)");
    }
    if (dumb_cap != 1) {
        fprintf(stderr, "FAIL: DRM_CAP_DUMB_BUFFER=%llu\n", (unsigned long long)dumb_cap);
        close(fd);
        return 1;
    }

    drmModeResPtr resources = drmModeGetResources(fd);
    if (resources == NULL) {
        close(fd);
        return fail_msg("drmModeGetResources returned NULL");
    }
    if (resources->count_crtcs != 1 || resources->count_connectors != 1 || resources->count_encoders != 1) {
        fprintf(stderr, "FAIL: unexpected KMS topology crtcs=%d connectors=%d encoders=%d\n",
                resources->count_crtcs, resources->count_connectors, resources->count_encoders);
        drmModeFreeResources(resources);
        close(fd);
        return 1;
    }
    if (resources->max_width <= 0 || resources->max_height <= 0 ||
        resources->max_width < resources->min_width || resources->max_height < resources->min_height) {
        fprintf(stderr, "FAIL: invalid KMS size bounds min=%dx%d max=%dx%d\n", resources->min_width,
                resources->min_height, resources->max_width, resources->max_height);
        drmModeFreeResources(resources);
        close(fd);
        return 1;
    }

    drmModeConnectorPtr connector = drmModeGetConnector(fd, resources->connectors[0]);
    if (connector == NULL) {
        drmModeFreeResources(resources);
        close(fd);
        return fail_msg("drmModeGetConnector returned NULL");
    }
    if (connector->connection != DRM_MODE_CONNECTED || connector->connector_type != DRM_MODE_CONNECTOR_VIRTUAL ||
        connector->count_modes != 1 || connector->count_encoders != 1) {
        fprintf(stderr,
                "FAIL: connector state connection=%d type=%u modes=%d encoders=%d\n",
                connector->connection, connector->connector_type, connector->count_modes,
                connector->count_encoders);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(fd);
        return 1;
    }

    drmModeEncoderPtr encoder = drmModeGetEncoder(fd, connector->encoders[0]);
    if (encoder == NULL) {
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(fd);
        return fail_msg("drmModeGetEncoder returned NULL");
    }
    if (encoder->encoder_type != DRM_MODE_ENCODER_VIRTUAL || encoder->crtc_id != (uint32_t)resources->crtcs[0] ||
        (encoder->possible_crtcs & 0x1U) == 0) {
        fprintf(stderr, "FAIL: encoder state type=%u crtc_id=%u possible_crtcs=%u\n",
                encoder->encoder_type, encoder->crtc_id, encoder->possible_crtcs);
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(fd);
        return 1;
    }

    drmModeCrtcPtr crtc = drmModeGetCrtc(fd, encoder->crtc_id);
    if (crtc == NULL) {
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(fd);
        return fail_msg("drmModeGetCrtc returned NULL");
    }
    if (crtc->crtc_id != encoder->crtc_id || !mode_matches_crtc(&connector->modes[0], &crtc->mode)) {
        fprintf(stderr, "FAIL: CRTC state crtc_id=%u encoder_crtc=%u mode=%s connector_mode=%s\n",
                crtc->crtc_id, encoder->crtc_id, crtc->mode.name, connector->modes[0].name);
        drmModeFreeCrtc(crtc);
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(fd);
        return 1;
    }

    printf("KMS connector %u mode %s %ux%u via encoder %u crtc %u\n", connector->connector_id,
           connector->modes[0].name, connector->modes[0].hdisplay, connector->modes[0].vdisplay,
           encoder->encoder_id, crtc->crtc_id);

    drmModePlaneResPtr plane_resources = drmModeGetPlaneResources(fd);
    if (plane_resources == NULL) {
        drmModeFreeCrtc(crtc);
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(fd);
        return fail_msg("drmModeGetPlaneResources returned NULL");
    }
    if (plane_resources->count_planes != 1) {
        fprintf(stderr, "FAIL: unexpected plane count=%u\n", plane_resources->count_planes);
        drmModeFreePlaneResources(plane_resources);
        drmModeFreeCrtc(crtc);
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(fd);
        return 1;
    }

    drmModePlanePtr plane = drmModeGetPlane(fd, plane_resources->planes[0]);
    if (plane == NULL) {
        drmModeFreePlaneResources(plane_resources);
        drmModeFreeCrtc(crtc);
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(fd);
        return fail_msg("drmModeGetPlane returned NULL");
    }
    if (plane->crtc_id != crtc->crtc_id || (plane->possible_crtcs & 0x1U) == 0 ||
        !plane_supports_format(plane, DRM_FORMAT_XRGB8888) ||
        !plane_supports_format(plane, DRM_FORMAT_ARGB8888)) {
        fprintf(stderr, "FAIL: plane state id=%u crtc=%u possible=%#x formats=%u\n",
                plane->plane_id, plane->crtc_id, plane->possible_crtcs, plane->count_formats);
        drmModeFreePlane(plane);
        drmModeFreePlaneResources(plane_resources);
        drmModeFreeCrtc(crtc);
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(fd);
        return 1;
    }

    drmModeObjectPropertiesPtr plane_props =
        drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
    drmModeObjectPropertiesPtr crtc_props =
        drmModeObjectGetProperties(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC);
    drmModeObjectPropertiesPtr connector_props =
        drmModeObjectGetProperties(fd, connector->connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (plane_props == NULL || crtc_props == NULL || connector_props == NULL ||
        plane_props->count_props != 0 || crtc_props->count_props != 0 ||
        connector_props->count_props != 0) {
        fprintf(stderr,
                "FAIL: object properties plane=%p/%u crtc=%p/%u connector=%p/%u\n",
                (void *)plane_props, plane_props != NULL ? plane_props->count_props : UINT32_MAX,
                (void *)crtc_props, crtc_props != NULL ? crtc_props->count_props : UINT32_MAX,
                (void *)connector_props,
                connector_props != NULL ? connector_props->count_props : UINT32_MAX);
        if (connector_props != NULL) {
            drmModeFreeObjectProperties(connector_props);
        }
        if (crtc_props != NULL) {
            drmModeFreeObjectProperties(crtc_props);
        }
        if (plane_props != NULL) {
            drmModeFreeObjectProperties(plane_props);
        }
        drmModeFreePlane(plane);
        drmModeFreePlaneResources(plane_resources);
        drmModeFreeCrtc(crtc);
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        close(fd);
        return 1;
    }
    printf("KMS plane %u supports XRGB8888/ARGB8888 and exposes no properties yet\n",
           plane->plane_id);

    drmModeFreeObjectProperties(connector_props);
    drmModeFreeObjectProperties(crtc_props);
    drmModeFreeObjectProperties(plane_props);
    drmModeFreePlane(plane);
    drmModeFreePlaneResources(plane_resources);
    drmModeFreeCrtc(crtc);
    drmModeFreeEncoder(encoder);
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    close(fd);

    printf("libdrm KMS topology tests passed\n");
    return 0;
}
