#include <gtk/gtk.h>
#include <stdio.h>

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

int main(void) {
    if (check_gtk_version() != 0 || check_type_system() != 0 || check_string_list() != 0 ||
        check_gdk_and_gsk_helpers() != 0) {
        return 1;
    }

    GdkDisplay *display = gdk_display_get_default();
    g_print("GTK display availability: %s\n", display == NULL ? "none" : "present");
    g_print("GTK version, type system, model and GDK/GSK helper tests passed\n");
    g_print("All GTK smoke tests passed!\n");
    return 0;
}
