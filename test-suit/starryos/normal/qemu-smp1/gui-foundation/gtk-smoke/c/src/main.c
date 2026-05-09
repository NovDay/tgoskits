#define _GNU_SOURCE

#include <errno.h>
#include <gtk/gtk.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>

#include "xdg-shell-protocol.h"

struct MiniWaylandServer {
    struct wl_display *display;
    struct wl_global *compositor_global;
    struct wl_global *data_device_manager_global;
    struct wl_global *output_global;
    struct wl_global *seat_global;
    struct wl_global *subcompositor_global;
    struct wl_global *xdg_wm_base_global;
    struct wl_event_source *listener_source;
    GThread *thread;
    gint running;
    int listener_fd;
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

struct MiniSurface {
    struct wl_resource *surface_resource;
    struct wl_resource *xdg_surface_resource;
    bool configured;
};

static void fail_on_timeout(int signo) {
    (void)signo;
    const char message[] = "FAIL: GTK smoke timed out\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static void noop_resource_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void noop_resource_cleanup(struct wl_resource *resource) {
    (void)resource;
}

static void noop_resource_request(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    (void)resource;
}

static void destroy_mini_surface(struct wl_resource *resource) {
    struct MiniSurface *surface = wl_resource_get_user_data(resource);
    if (surface == NULL) {
        return;
    }
    if (surface->surface_resource == resource) {
        surface->surface_resource = NULL;
    }
    if (surface->xdg_surface_resource == resource) {
        surface->xdg_surface_resource = NULL;
    }
    if (surface->surface_resource == NULL && surface->xdg_surface_resource == NULL) {
        free(surface);
    }
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

static void surface_set_region(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *region) {
    (void)client;
    (void)resource;
    (void)region;
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    struct MiniSurface *surface = wl_resource_get_user_data(resource);
    if (surface != NULL && surface->xdg_surface_resource != NULL && !surface->configured) {
        const uint32_t serial = 1;
        xdg_surface_send_configure(surface->xdg_surface_resource, serial);
        surface->configured = true;
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

static void surface_offset(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t x,
    int32_t y) {
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
}

static const struct wl_surface_interface surface_impl = {
    .destroy = noop_resource_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_region,
    .set_input_region = surface_set_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
};

static void compositor_create_surface(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id) {
    (void)resource;
    struct MiniSurface *surface = calloc(1, sizeof(*surface));
    if (surface == NULL) {
        wl_client_post_no_memory(client);
        return;
    }

    surface->surface_resource = wl_resource_create(client, &wl_surface_interface, 6, id);
    if (surface->surface_resource == NULL) {
        free(surface);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(surface->surface_resource, &surface_impl, surface,
                                   destroy_mini_surface);
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

static const struct wl_region_interface region_impl = {
    .destroy = noop_resource_destroy,
    .add = region_add,
    .subtract = region_add,
};

static void compositor_create_region(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id) {
    (void)resource;
    struct wl_resource *region =
        wl_resource_create(client, &wl_region_interface, 1, id);
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
    (void)data;
    uint32_t bind_version = version < 6 ? version : 6;
    struct wl_resource *resource =
        wl_resource_create(client, &wl_compositor_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &compositor_impl, NULL, NULL);
}

static void data_source_offer(
    struct wl_client *client,
    struct wl_resource *resource,
    const char *mime_type) {
    (void)client;
    (void)resource;
    (void)mime_type;
}

static void data_source_set_actions(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t dnd_actions) {
    (void)client;
    (void)resource;
    (void)dnd_actions;
}

static const struct wl_data_source_interface data_source_impl = {
    .offer = data_source_offer,
    .destroy = noop_resource_destroy,
    .set_actions = data_source_set_actions,
};

static void data_device_start_drag(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *source,
    struct wl_resource *origin,
    struct wl_resource *icon,
    uint32_t serial) {
    (void)client;
    (void)resource;
    (void)source;
    (void)origin;
    (void)icon;
    (void)serial;
}

static void data_device_set_selection(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *source,
    uint32_t serial) {
    (void)client;
    (void)resource;
    (void)source;
    (void)serial;
}

static const struct wl_data_device_interface data_device_impl = {
    .start_drag = data_device_start_drag,
    .set_selection = data_device_set_selection,
    .release = noop_resource_destroy,
};

static void pointer_set_cursor(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t serial,
    struct wl_resource *surface,
    int32_t hotspot_x,
    int32_t hotspot_y) {
    (void)client;
    (void)resource;
    (void)serial;
    (void)surface;
    (void)hotspot_x;
    (void)hotspot_y;
}

static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = pointer_set_cursor,
    .release = noop_resource_destroy,
};

static const struct wl_keyboard_interface keyboard_impl = {
    .release = noop_resource_destroy,
};

static const struct wl_touch_interface touch_impl = {
    .release = noop_resource_destroy,
};

static void data_device_manager_create_data_source(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id) {
    struct wl_resource *source =
        wl_resource_create(client, &wl_data_source_interface, wl_resource_get_version(resource), id);
    if (source == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(source, &data_source_impl, NULL, NULL);
}

static void data_device_manager_get_data_device(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id,
    struct wl_resource *seat) {
    (void)seat;
    struct wl_resource *device =
        wl_resource_create(client, &wl_data_device_interface, wl_resource_get_version(resource), id);
    if (device == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(device, &data_device_impl, NULL, NULL);
}

static const struct wl_data_device_manager_interface data_device_manager_impl = {
    .create_data_source = data_device_manager_create_data_source,
    .get_data_device = data_device_manager_get_data_device,
};

static void bind_data_device_manager(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id) {
    (void)data;
    uint32_t bind_version = version < 3 ? version : 3;
    struct wl_resource *resource =
        wl_resource_create(client, &wl_data_device_manager_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &data_device_manager_impl, NULL, NULL);
}

static void bind_output(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id) {
    (void)data;
    uint32_t bind_version = version < 4 ? version : 4;
    struct wl_resource *resource = wl_resource_create(client, &wl_output_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_output_send_geometry(resource, 0, 0, 340, 210, WL_OUTPUT_SUBPIXEL_UNKNOWN, "Starry",
                            "Mini Wayland", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, 1280, 800,
                        60000);
    if (bind_version >= 2) {
        wl_output_send_scale(resource, 1);
        wl_output_send_done(resource);
    }
}

static void subsurface_set_position(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t x,
    int32_t y) {
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
}

static void subsurface_place(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *sibling) {
    (void)client;
    (void)resource;
    (void)sibling;
}

static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = noop_resource_destroy,
    .set_position = subsurface_set_position,
    .place_above = subsurface_place,
    .place_below = subsurface_place,
    .set_sync = noop_resource_request,
    .set_desync = noop_resource_request,
};

static void subcompositor_get_subsurface(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id,
    struct wl_resource *surface,
    struct wl_resource *parent) {
    (void)resource;
    (void)surface;
    (void)parent;
    struct wl_resource *subsurface =
        wl_resource_create(client, &wl_subsurface_interface, 1, id);
    if (subsurface == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(subsurface, &subsurface_impl, NULL, NULL);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy = noop_resource_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void bind_subcompositor(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id) {
    (void)data;
    (void)version;
    struct wl_resource *resource = wl_resource_create(client, &wl_subcompositor_interface, 1, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &subcompositor_impl, NULL, NULL);
}

static void seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct wl_resource *pointer =
        wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    if (pointer == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(pointer, &pointer_impl, NULL, NULL);
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct wl_resource *keyboard =
        wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    if (keyboard == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(keyboard, &keyboard_impl, NULL, NULL);
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct wl_resource *touch =
        wl_resource_create(client, &wl_touch_interface, wl_resource_get_version(resource), id);
    if (touch == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(touch, &touch_impl, NULL, NULL);
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = noop_resource_destroy,
};

static void bind_seat(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id) {
    (void)data;
    uint32_t bind_version = version < 9 ? version : 9;
    struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER);
    if (bind_version >= 2) {
        wl_seat_send_name(resource, "seat0");
    }
}

static void xdg_toplevel_set_parent(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *parent) {
    (void)client;
    (void)resource;
    (void)parent;
}

static void xdg_toplevel_set_string(
    struct wl_client *client,
    struct wl_resource *resource,
    const char *value) {
    (void)client;
    (void)resource;
    (void)value;
}

static void xdg_toplevel_show_window_menu(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *seat,
    uint32_t serial,
    int32_t x,
    int32_t y) {
    (void)client;
    (void)resource;
    (void)seat;
    (void)serial;
    (void)x;
    (void)y;
}

static void xdg_toplevel_interactive(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *seat,
    uint32_t serial) {
    (void)client;
    (void)resource;
    (void)seat;
    (void)serial;
}

static void xdg_toplevel_resize(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *seat,
    uint32_t serial,
    uint32_t edges) {
    (void)client;
    (void)resource;
    (void)seat;
    (void)serial;
    (void)edges;
}

static void xdg_popup_grab(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *seat,
    uint32_t serial) {
    (void)client;
    (void)resource;
    (void)seat;
    (void)serial;
}

static void xdg_popup_reposition(
    struct wl_client *client,
    struct wl_resource *resource,
    struct wl_resource *positioner,
    uint32_t token) {
    (void)client;
    (void)resource;
    (void)positioner;
    (void)token;
}

static void xdg_toplevel_set_size(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t width,
    int32_t height) {
    (void)client;
    (void)resource;
    (void)width;
    (void)height;
}

static void xdg_toplevel_set_simple(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    (void)resource;
}

static void xdg_positioner_set_size(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t width,
    int32_t height) {
    (void)client;
    (void)resource;
    (void)width;
    (void)height;
}

static void xdg_positioner_set_anchor_rect(
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

static void xdg_positioner_set_enum(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t value) {
    (void)client;
    (void)resource;
    (void)value;
}

static void xdg_positioner_set_offset(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t x,
    int32_t y) {
    (void)client;
    (void)resource;
    (void)x;
    (void)y;
}

static void xdg_positioner_set_parent_size(
    struct wl_client *client,
    struct wl_resource *resource,
    int32_t parent_width,
    int32_t parent_height) {
    (void)client;
    (void)resource;
    (void)parent_width;
    (void)parent_height;
}

static const struct xdg_positioner_interface xdg_positioner_impl = {
    .destroy = noop_resource_destroy,
    .set_size = xdg_positioner_set_size,
    .set_anchor_rect = xdg_positioner_set_anchor_rect,
    .set_anchor = xdg_positioner_set_enum,
    .set_gravity = xdg_positioner_set_enum,
    .set_constraint_adjustment = xdg_positioner_set_enum,
    .set_offset = xdg_positioner_set_offset,
    .set_reactive = noop_resource_request,
    .set_parent_size = xdg_positioner_set_parent_size,
    .set_parent_configure = xdg_positioner_set_enum,
};

static const struct xdg_toplevel_interface xdg_toplevel_impl = {
    .destroy = noop_resource_destroy,
    .set_parent = xdg_toplevel_set_parent,
    .set_title = xdg_toplevel_set_string,
    .set_app_id = xdg_toplevel_set_string,
    .show_window_menu = xdg_toplevel_show_window_menu,
    .move = xdg_toplevel_interactive,
    .resize = xdg_toplevel_resize,
    .set_max_size = xdg_toplevel_set_size,
    .set_min_size = xdg_toplevel_set_size,
    .set_maximized = xdg_toplevel_set_simple,
    .unset_maximized = xdg_toplevel_set_simple,
    .set_fullscreen = xdg_toplevel_set_parent,
    .unset_fullscreen = xdg_toplevel_set_simple,
    .set_minimized = xdg_toplevel_set_simple,
};

static const struct xdg_popup_interface xdg_popup_impl = {
    .destroy = noop_resource_destroy,
    .grab = xdg_popup_grab,
    .reposition = xdg_popup_reposition,
};

static void xdg_surface_get_toplevel(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id) {
    struct wl_resource *toplevel =
        wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    if (toplevel == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(toplevel, &xdg_toplevel_impl, NULL, NULL);
    struct wl_array states;
    wl_array_init(&states);
    xdg_toplevel_send_configure(toplevel, 640, 480, &states);
    wl_array_release(&states);
}

static void xdg_surface_get_popup(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id,
    struct wl_resource *parent,
    struct wl_resource *positioner) {
    (void)resource;
    (void)parent;
    (void)positioner;
    struct wl_resource *popup =
        wl_resource_create(client, &xdg_popup_interface, 1, id);
    if (popup == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(popup, &xdg_popup_impl, NULL, NULL);
}

static void xdg_surface_set_window_geometry(
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

static void xdg_surface_ack_configure(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t serial) {
    (void)client;
    (void)resource;
    (void)serial;
}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = noop_resource_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

static void xdg_wm_base_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_wm_base_create_positioner(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id) {
    (void)resource;
    struct wl_resource *positioner =
        wl_resource_create(client, &xdg_positioner_interface, 1, id);
    if (positioner == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(positioner, &xdg_positioner_impl, NULL, noop_resource_cleanup);
}

static void xdg_wm_base_get_xdg_surface(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id,
    struct wl_resource *surface_resource) {
    (void)resource;
    struct MiniSurface *surface = wl_resource_get_user_data(surface_resource);
    if (surface == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    surface->xdg_surface_resource =
        wl_resource_create(client, &xdg_surface_interface, 1, id);
    if (surface->xdg_surface_resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(surface->xdg_surface_resource, &xdg_surface_impl, surface,
                                   destroy_mini_surface);
}

static void xdg_wm_base_pong(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t serial) {
    (void)client;
    (void)resource;
    (void)serial;
}

static const struct xdg_wm_base_interface xdg_wm_base_impl = {
    .destroy = xdg_wm_base_destroy,
    .create_positioner = xdg_wm_base_create_positioner,
    .get_xdg_surface = xdg_wm_base_get_xdg_surface,
    .pong = xdg_wm_base_pong,
};

static void bind_xdg_wm_base(
    struct wl_client *client,
    void *data,
    uint32_t version,
    uint32_t id) {
    (void)data;
    uint32_t bind_version = version < 7 ? version : 7;
    struct wl_resource *resource =
        wl_resource_create(client, &xdg_wm_base_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &xdg_wm_base_impl, NULL, NULL);
}

static int accept_wayland_client(int fd, uint32_t mask, void *data) {
    struct MiniWaylandServer *server = data;

    if ((mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) != 0) {
        g_printerr("FAIL: GTK Wayland listener event mask=0x%x\n", mask);
        return 0;
    }

    while ((mask & WL_EVENT_READABLE) != 0) {
        int client_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            g_printerr("FAIL: accept GTK Wayland client: %s\n", strerror(errno));
            return 0;
        }
        if (wl_client_create(server->display, client_fd) == NULL) {
            close(client_fd);
            g_printerr("FAIL: wl_client_create for GTK returned NULL\n");
            return 0;
        }
    }
    return 0;
}

static int init_mini_wayland_server(struct MiniWaylandServer *server) {
    server->listener_fd = -1;
    server->display = wl_display_create();
    if (server->display == NULL) {
        g_printerr("FAIL: wl_display_create for GTK returned NULL\n");
        return 1;
    }
    if (wl_display_init_shm(server->display) != 0) {
        g_printerr("FAIL: wl_display_init_shm for GTK: %s\n", strerror(errno));
        return 1;
    }

    server->compositor_global =
        wl_global_create(server->display, &wl_compositor_interface, 6, server, bind_compositor);
    server->data_device_manager_global = wl_global_create(
        server->display, &wl_data_device_manager_interface, 3, server, bind_data_device_manager);
    server->output_global =
        wl_global_create(server->display, &wl_output_interface, 4, server, bind_output);
    server->seat_global =
        wl_global_create(server->display, &wl_seat_interface, 9, server, bind_seat);
    server->subcompositor_global = wl_global_create(
        server->display, &wl_subcompositor_interface, 1, server, bind_subcompositor);
    server->xdg_wm_base_global =
        wl_global_create(server->display, &xdg_wm_base_interface, 7, server, bind_xdg_wm_base);
    if (server->compositor_global == NULL || server->data_device_manager_global == NULL ||
        server->output_global == NULL || server->seat_global == NULL ||
        server->subcompositor_global == NULL || server->xdg_wm_base_global == NULL) {
        g_printerr("FAIL: create GTK Wayland globals\n");
        return 1;
    }

    server->listener_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (server->listener_fd < 0) {
        g_printerr("FAIL: GTK Wayland listener socket: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(server->socket_path, sizeof(server->socket_path), "/tmp/gtk-smoke-%u.sock",
             (unsigned)getpid());
    strncpy(addr.sun_path, server->socket_path, sizeof(addr.sun_path) - 1);
    unlink(server->socket_path);

    if (bind(server->listener_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        g_printerr("FAIL: bind GTK Wayland listener: %s\n", strerror(errno));
        return 1;
    }
    if (listen(server->listener_fd, 128) != 0) {
        g_printerr("FAIL: listen GTK Wayland listener: %s\n", strerror(errno));
        return 1;
    }

    server->listener_source = wl_event_loop_add_fd(
        wl_display_get_event_loop(server->display), server->listener_fd, WL_EVENT_READABLE,
        accept_wayland_client, server);
    if (server->listener_source == NULL) {
        g_printerr("FAIL: wl_event_loop_add_fd for GTK listener returned NULL\n");
        return 1;
    }

    if (setenv("XDG_RUNTIME_DIR", "/tmp", 1) != 0 ||
        setenv("WAYLAND_DISPLAY", server->socket_path, 1) != 0 ||
        setenv("GDK_BACKEND", "wayland", 1) != 0) {
        g_printerr("FAIL: set GTK Wayland environment: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static gpointer run_mini_wayland_server(gpointer data) {
    struct MiniWaylandServer *server = data;
    struct wl_event_loop *loop = wl_display_get_event_loop(server->display);

    while (g_atomic_int_get(&server->running) != 0) {
        wl_event_loop_dispatch(loop, 10);
        wl_display_flush_clients(server->display);
    }

    return NULL;
}

static int start_mini_wayland_server(struct MiniWaylandServer *server) {
    g_atomic_int_set(&server->running, 1);
    server->thread = g_thread_new("gtk-smoke-wayland-server", run_mini_wayland_server, server);
    if (server->thread == NULL) {
        g_printerr("FAIL: g_thread_new for GTK Wayland server returned NULL\n");
        g_atomic_int_set(&server->running, 0);
        return 1;
    }
    return 0;
}

static void cleanup_mini_wayland_server(struct MiniWaylandServer *server) {
    g_atomic_int_set(&server->running, 0);
    if (server->thread != NULL) {
        g_thread_join(server->thread);
    }
    if (server->display != NULL) {
        wl_display_destroy_clients(server->display);
    }
    if (server->listener_source != NULL) {
        wl_event_source_remove(server->listener_source);
    }
    if (server->xdg_wm_base_global != NULL) {
        wl_global_destroy(server->xdg_wm_base_global);
    }
    if (server->subcompositor_global != NULL) {
        wl_global_destroy(server->subcompositor_global);
    }
    if (server->seat_global != NULL) {
        wl_global_destroy(server->seat_global);
    }
    if (server->output_global != NULL) {
        wl_global_destroy(server->output_global);
    }
    if (server->data_device_manager_global != NULL) {
        wl_global_destroy(server->data_device_manager_global);
    }
    if (server->compositor_global != NULL) {
        wl_global_destroy(server->compositor_global);
    }
    if (server->display != NULL) {
        wl_display_destroy(server->display);
    }
    if (server->listener_fd >= 0) {
        close(server->listener_fd);
    }
    if (server->socket_path[0] != '\0') {
        unlink(server->socket_path);
    }
}

static int check_gtk_version(void) {
    guint major = gtk_get_major_version();
    guint minor = gtk_get_minor_version();
    guint micro = gtk_get_micro_version();

    if (major < 4) {
        g_printerr("FAIL: GTK major version=%u.%u.%u\n", major, minor, micro);
        return 1;
    }

    const char *mismatch = gtk_check_version(4, 0, 0);
    if (mismatch != NULL) {
        g_printerr("FAIL: gtk_check_version mismatch: %s\n", mismatch);
        return 1;
    }

    return 0;
}

static int check_type_system(void) {
    GType widget_type = GTK_TYPE_WIDGET;
    GType string_list_type = GTK_TYPE_STRING_LIST;

    if (!G_TYPE_IS_OBJECT(widget_type) || !G_TYPE_IS_OBJECT(string_list_type)) {
        g_printerr("FAIL: GTK object type checks failed widget=%lu string_list=%lu\n",
                   (unsigned long)widget_type, (unsigned long)string_list_type);
        return 1;
    }

    return 0;
}

static int check_string_list(void) {
    const char *items[] = {"starry", "gtk", NULL};
    GtkStringList *list = gtk_string_list_new(items);
    if (list == NULL) {
        g_printerr("FAIL: gtk_string_list_new returned NULL\n");
        return 1;
    }

    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(list));
    const char *second = gtk_string_list_get_string(list, 1);
    if (n_items != 2 || second == NULL || g_strcmp0(second, "gtk") != 0) {
        g_printerr("FAIL: GtkStringList n_items=%u second=%s\n", n_items,
                   second == NULL ? "(null)" : second);
        g_object_unref(list);
        return 1;
    }

    g_object_unref(list);
    return 0;
}

static int check_gdk_and_gsk_helpers(void) {
    GdkRGBA color;
    if (!gdk_rgba_parse(&color, "#336699")) {
        g_printerr("FAIL: gdk_rgba_parse failed\n");
        return 1;
    }
    if (color.red <= 0.0 || color.blue <= 0.0 || color.alpha != 1.0) {
        g_printerr("FAIL: parsed GdkRGBA red=%f blue=%f alpha=%f\n", color.red, color.blue,
                   color.alpha);
        return 1;
    }

    graphene_rect_t bounds;
    graphene_rect_init(&bounds, 0.0f, 0.0f, 64.0f, 32.0f);

    GskRoundedRect rounded;
    gsk_rounded_rect_init_from_rect(&rounded, &bounds, 4.0f);
    if (rounded.bounds.size.width != 64.0f || rounded.bounds.size.height != 32.0f) {
        g_printerr("FAIL: GskRoundedRect size=%fx%f\n", rounded.bounds.size.width,
                   rounded.bounds.size.height);
        return 1;
    }

    return 0;
}

static int check_gtk_wayland_display(void) {
    struct MiniWaylandServer server = {0};
    if (init_mini_wayland_server(&server) != 0 || start_mini_wayland_server(&server) != 0) {
        cleanup_mini_wayland_server(&server);
        return 1;
    }

    gboolean gtk_initialized = gtk_init_check();
    if (!gtk_initialized) {
        g_printerr("FAIL: gtk_init_check did not connect to mini Wayland display\n");
        cleanup_mini_wayland_server(&server);
        return 1;
    }

    GdkDisplay *display = gdk_display_get_default();
    if (display == NULL) {
        g_printerr("FAIL: gdk_display_get_default returned NULL after gtk_init_check\n");
        cleanup_mini_wayland_server(&server);
        return 1;
    }

    const char *display_name = gdk_display_get_name(display);
    if (display_name == NULL || strstr(display_name, "gtk-smoke-") == NULL) {
        g_printerr("FAIL: unexpected GTK display name: %s\n",
                   display_name == NULL ? "(null)" : display_name);
        cleanup_mini_wayland_server(&server);
        return 1;
    }

    GtkWidget *window = gtk_window_new();
    if (window == NULL) {
        g_printerr("FAIL: gtk_window_new returned NULL\n");
        cleanup_mini_wayland_server(&server);
        return 1;
    }
    gtk_window_set_default_size(GTK_WINDOW(window), 320, 200);
    gtk_window_set_title(GTK_WINDOW(window), "Starry GTK smoke");

    g_object_unref(window);
    g_print("GTK Wayland backend initialized through mini display: %s\n", display_name);
    cleanup_mini_wayland_server(&server);
    return 0;
}

int main(void) {
    signal(SIGALRM, fail_on_timeout);
    alarm(15);

    if (check_gtk_version() != 0 || check_type_system() != 0 || check_string_list() != 0 ||
        check_gdk_and_gsk_helpers() != 0 || check_gtk_wayland_display() != 0) {
        return 1;
    }

    GdkDisplay *display = gdk_display_get_default();
    g_print("GTK display availability: %s\n", display == NULL ? "none" : "present");
    g_print("GTK version, type system, model, GDK/GSK helper and Wayland init tests passed\n");
    g_print("All GTK smoke tests passed!\n");
    alarm(0);
    return 0;
}
