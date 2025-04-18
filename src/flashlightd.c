/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 */

#include <gst/gst.h>
#include <gio/gio.h>
#include <stdio.h>
#include <fcntl.h>

#define FLASHLIGHTD_BUS_NAME "io.furios.Flashlightd"
#define FLASHLIGHTD_OBJECT_PATH "/io/furios/Flashlightd"
#define FLASHLIGHTD_INTERFACE "io.furios.Flashlightd"
#define PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"

static const gchar *introspection_xml =
  "<node>"
  "  <interface name='io.furios.Flashlightd'>"
  "    <method name='SetBrightness'>"
  "      <arg type='u' name='brightness' direction='in'/>"
  "    </method>"
  "    <property name='Brightness' type='u' access='read'/>"
  "    <property name='MaxBrightness' type='u' access='read'/>"
  "    <property name='Scalable' type='b' access='read'/>"
  "  </interface>"
  "</node>";

typedef struct {
    GDBusConnection *connection;
    guint registration_id;
    GSList *sysfs_path;
    GstElement *pipeline;
    guint brightness;
    guint max_brightness;
    gboolean scalable;
    gboolean use_sysfs;
    gchar *scalable_path;
    guint scalable_max_brightness;
} FlashlightServer;

static FlashlightServer *server = NULL;

static gboolean
read_file_contents(const gchar *path, gchar **content, gsize *length)
{
    g_debug("Reading file: %s", path);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_debug("File does not exist: %s", path);
        return FALSE;
    }

    return g_file_get_contents(path, content, length, NULL);
}

static void
write_to_file(const char *path, const char *value)
{
    g_debug("Writing to %s: %s", path, value);
    int fd = open(path, O_WRONLY);
    if (fd == -1) {
        perror("open");
        return;
    }

    if (write(fd, value, strlen(value)) == -1)
        perror("write");

    close(fd);
}

static guint
read_brightness_value(const gchar *path)
{
    gchar *content = NULL;
    gsize length = 0;
    guint value = 0;

    if (read_file_contents(path, &content, &length)) {
        value = atoi(g_strstrip(content));
        g_free(content);
    }

    return value;
}

static void
add_sysfs_paths(FlashlightServer *server, const gchar **paths, int count)
{
    for (int i = 0; i < count; i++) {
        server->sysfs_path = g_slist_append(server->sysfs_path, g_strdup(paths[i]));
    }
}

static void
load_custom_paths(FlashlightServer *server, const gchar *custom_file)
{
    gchar *content = NULL;
    gsize length = 0;

    if (read_file_contents(custom_file, &content, &length)) {
        g_debug("Reading custom paths from %s", custom_file);
        gchar **nodes = g_strsplit(content, ",", -1);
        int count = 0;

        for (int i = 0; nodes[i] != NULL; i++) {
            gchar *trimmed = g_strstrip(nodes[i]);
            if (strlen(trimmed) > 0) {
                server->sysfs_path = g_slist_append(server->sysfs_path, g_strdup(trimmed));
                count++;
                g_debug("Added custom path: %s", trimmed);
            }
        }

        g_debug("Added %d custom paths", count);
        g_strfreev(nodes);
        g_free(content);
    }
}

static void
initialize_sysfs_paths(FlashlightServer *server)
{
    /* Default sysfs paths */
    const gchar *default_paths[] = {
        "/sys/class/leds/torch-light/brightness",
        "/sys/class/leds/flashlight/brightness",
        "/sys/class/leds/torch-light0/brightness",
        "/sys/class/leds/torch-light1/brightness",
        "/sys/class/leds/led:flash_torch/brightness",
        "/sys/class/leds/led:flash_0/brightness",
        "/sys/class/leds/led:flash_1/brightness",
        "/sys/class/leds/led:flash_2/brightness",
        "/sys/class/leds/led:flash_3/brightness",
        "/sys/class/leds/led:torch_0/brightness",
        "/sys/class/leds/led:torch_1/brightness",
        "/sys/class/leds/led:torch_2/brightness",
        "/sys/class/leds/led:torch_3/brightness",
        "/sys/class/leds/led:switch/brightness",
        "/sys/class/leds/led:switch_0/brightness",
        "/sys/class/leds/led:switch_1/brightness",
        "/sys/class/leds/led:switch_2/brightness",
        "/sys/devices/platform/soc/soc:i2c@1/i2c-23/23-0059/s2mpb02-led/leds/torch-sec1/brightness",
        "/sys/class/flashlight_core/flashlight/flashlight_torch"
    };

    add_sysfs_paths(server, default_paths, sizeof(default_paths) / sizeof(default_paths[0]));
    g_debug("Added %d default sysfs paths", g_slist_length(server->sysfs_path));

    /* Allow to override the sysfs list */
    const gchar *custom_nodes_file = "/usr/lib/furios/device/flashlightd-sysfs";
    if (g_file_test(custom_nodes_file, G_FILE_TEST_EXISTS))
        load_custom_paths(server, custom_nodes_file);
    else
        g_debug("No custom sysfs configuration file found at %s", custom_nodes_file);

    /* Count valid paths */
    int count = 0;
    GSList *current = server->sysfs_path;
    while (current != NULL) {
        const gchar *path = (const gchar *)current->data;
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            g_debug("Found valid sysfs path: %s", path);
            count++;
        }
        current = current->next;
    }
    g_debug("Found %d valid sysfs paths out of %d total paths", count, g_slist_length(server->sysfs_path));
}

static gchar *
get_path_from_file(const gchar *file_path)
{
    if (!g_file_test(file_path, G_FILE_TEST_EXISTS)) {
        g_debug("Config file does not exist: %s", file_path);
        return NULL;
    }

    gchar *content = NULL;
    gsize length = 0;
    gchar *result = NULL;

    if (read_file_contents(file_path, &content, &length)) {
        g_strstrip(content);
        g_debug("Path from %s: '%s'", file_path, content);

        if (g_file_test(content, G_FILE_TEST_EXISTS)) {
            result = g_strdup(content);
            g_debug("Found valid path: %s", result);
        } else {
            g_debug("Path does not exist: %s", content);
        }
        g_free(content);
    }

    return result;
}

static void
load_scalable_config(FlashlightServer *server)
{
    const gchar *scalable_path_file = "/usr/lib/furios/device/flashlightd-scalable-path";
    gchar *scalable_path = get_path_from_file(scalable_path_file);

    if (scalable_path != NULL) {
        server->scalable_path = scalable_path;
        g_debug("Using custom scalable path: %s", server->scalable_path);

        /* Load max brightness value */
        const gchar *scalable_max_file = "/usr/lib/furios/device/flashlightd-scalable-max-path";
        gchar *max_path = get_path_from_file(scalable_max_file);

        if (max_path != NULL) {
            guint max_value = read_brightness_value(max_path);
            if (max_value > 0) {
                server->scalable_max_brightness = max_value;
                server->max_brightness = max_value;
                g_debug("Using custom max brightness: %u from file: %s",
                      server->scalable_max_brightness, max_path);
            }
            g_free(max_path);
        }
    }
}

static void
initialize_max_brightness(FlashlightServer *server)
{
    server->max_brightness = 1;

    /* Find max brightness from LED sys files */
    GSList *current = server->sysfs_path;
    while (current != NULL) {
        const gchar *path = (const gchar *)current->data;
        gchar *max_path = g_strdup_printf("%s/../max_brightness", path);

        if (g_file_test(max_path, G_FILE_TEST_EXISTS)) {
            guint max_value = read_brightness_value(max_path);
            if (max_value > 0) {
                server->max_brightness = max_value;
                g_debug("Found max_brightness file: %s with value: %u", max_path, max_value);
                g_free(max_path);
                break;
            }
        }

        g_free(max_path);
        current = current->next;
    }

    if (server->max_brightness == 1)
        g_debug("No max_brightness file found, using default value: 1");

    /* Load scalable configuration */
    load_scalable_config(server);

    /* Determine if brightness is scalable */
    server->scalable = (server->max_brightness > 1);

    /* Allow to override scalable */
    if (g_file_test("/usr/lib/furios/device/flashlightd-no-scale", G_FILE_TEST_EXISTS))
        server->scalable = FALSE;
}

static void
cleanup_server(FlashlightServer *server)
{
    if (server->pipeline != NULL) {
        gst_element_set_state(server->pipeline, GST_STATE_NULL);
        gst_object_unref(server->pipeline);
    }

    GSList *current = server->sysfs_path;
    while (current != NULL) {
        g_free(current->data);
        current = current->next;
    }

    g_slist_free(server->sysfs_path);
    g_free(server->scalable_path);
    g_free(server);
}

static void
emit_property_changed(FlashlightServer *server, const gchar *property_name, GVariant *value)
{
    GVariantBuilder *builder;
    GVariantBuilder *invalidated_builder;

    builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(builder, "{sv}", property_name, value);

    invalidated_builder = g_variant_builder_new(G_VARIANT_TYPE("as"));

    g_dbus_connection_emit_signal(server->connection,
                                  NULL,
                                  FLASHLIGHTD_OBJECT_PATH,
                                  PROPERTIES_INTERFACE,
                                  "PropertiesChanged",
                                  g_variant_new("(sa{sv}as)",
                                                FLASHLIGHTD_INTERFACE,
                                                builder,
                                                invalidated_builder),
                                  NULL);

    g_variant_builder_unref(builder);
    g_variant_builder_unref(invalidated_builder);
}

static void
write_brightness_to_all_paths(FlashlightServer *server, guint value)
{
    GSList *current = server->sysfs_path;
    while (current != NULL) {
        const gchar *path = (const gchar *)current->data;

        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            gchar *brightness_str = g_strdup_printf("%u\n", value);
            write_to_file(path, brightness_str);
            g_debug("Writing %u to: %s", value, path);
            g_free(brightness_str);
        }

        current = current->next;
    }
}

static gboolean
set_flashlight_sysfs(FlashlightServer *server)
{
    gboolean success = FALSE;

    if (server->scalable_path != NULL && server->scalable) {
        /* Special case for devices with separate scaling node */
        if (server->brightness == 0) {
            write_brightness_to_all_paths(server, 0);
            success = TRUE;
        } else {
            write_brightness_to_all_paths(server, 1);

            /* Set the brightness using the scaling node */
            if (g_file_test(server->scalable_path, G_FILE_TEST_EXISTS)) {
                guint scaled_value = server->brightness;
                if (scaled_value > server->scalable_max_brightness)
                    scaled_value = server->scalable_max_brightness;

                gchar *brightness_str = g_strdup_printf("%u\n", scaled_value);
                write_to_file(server->scalable_path, brightness_str);
                g_free(brightness_str);
                g_debug("Setting brightness - wrote %u to scaling path: %s",
                        scaled_value, server->scalable_path);
            }
            success = TRUE;
        }
    } else {
        /* Single node for both turning on and scalable */
        GSList *current = server->sysfs_path;
        while (current != NULL) {
            const gchar *path = (const gchar *)current->data;

            if (g_file_test(path, G_FILE_TEST_EXISTS)) {
                guint value_to_write;

                if (server->scalable && server->brightness > 0) {
                    value_to_write = server->brightness;
                    if (value_to_write > server->max_brightness)
                        value_to_write = server->max_brightness;
                } else {
                    value_to_write = server->brightness > 0 ? 1 : 0;
                }

                gchar *brightness_str = g_strdup_printf("%u\n", value_to_write);
                write_to_file(path, brightness_str);
                g_debug("Writing %u to: %s", value_to_write, path);
                g_free(brightness_str);
                success = TRUE;
            }

            current = current->next;
        }
    }

    return success;
}

static void
set_flashlight_gstreamer(FlashlightServer *server)
{
    if (server->brightness == 0) {
        if (server->pipeline != NULL) {
            g_debug("Turning flashlight off via GStreamer");
            gst_element_set_state(server->pipeline, GST_STATE_NULL);
            gst_object_unref(server->pipeline);
            server->pipeline = NULL;
        }
    } else {
        if (server->pipeline == NULL) {
            g_debug("Turning flashlight on via GStreamer");
            GError *error = NULL;
            server->pipeline = gst_parse_launch("droidcamsrc video-torch=true mode=2 ! fakesink", &error);

            if (error != NULL) {
                g_warning("Failed to create GStreamer pipeline: %s", error->message);
                g_error_free(error);
                server->use_sysfs = TRUE;
                g_debug("Falling back to sysfs method");
                set_flashlight_sysfs(server);
                return;
            }

            GstStateChangeReturn ret = gst_element_set_state(server->pipeline, GST_STATE_PLAYING);
            if (ret == GST_STATE_CHANGE_FAILURE) {
                g_warning("Failed to set GStreamer pipeline to PLAYING state");
                gst_object_unref(server->pipeline);
                server->pipeline = NULL;
                server->use_sysfs = TRUE;
                g_debug("Falling back to sysfs method");
                set_flashlight_sysfs(server);
            }
        }
    }
}

static void
set_flashlight(FlashlightServer *server)
{
    if (server->use_sysfs)
        set_flashlight_sysfs(server);
    else
        set_flashlight_gstreamer(server);
}

static void
handle_method_call(GDBusConnection *connection,
                   const gchar *sender,
                   const gchar *object_path,
                   const gchar *interface_name,
                   const gchar *method_name,
                   GVariant *parameters,
                   GDBusMethodInvocation *invocation,
                   gpointer user_data)
{
    FlashlightServer *server = (FlashlightServer *)user_data;

    if (g_strcmp0(method_name, "SetBrightness") == 0) {
        guint brightness;
        g_variant_get(parameters, "(u)", &brightness);

        server->brightness = brightness;
        set_flashlight(server);

        emit_property_changed(server, "Brightness", g_variant_new_uint32(server->brightness));
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else {
        g_dbus_method_invocation_return_error(invocation,
                                              G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "Unknown method: %s", method_name);
    }
}

static GVariant *
handle_get_property(GDBusConnection *connection,
                    const gchar *sender,
                    const gchar *object_path,
                    const gchar *interface_name,
                    const gchar *property_name,
                    GError **error,
                    gpointer user_data)
{
    FlashlightServer *server = (FlashlightServer *)user_data;

    if (g_strcmp0(property_name, "Brightness") == 0)
        return g_variant_new_uint32(server->brightness);
    else if (g_strcmp0(property_name, "MaxBrightness") == 0)
        return g_variant_new_uint32(server->max_brightness);
    else if (g_strcmp0(property_name, "Scalable") == 0)
        return g_variant_new_boolean(server->scalable);

    g_set_error(error,
                G_DBUS_ERROR,
                G_DBUS_ERROR_UNKNOWN_PROPERTY,
                "Unknown property: %s", property_name);
    return NULL;
}

static gboolean
handle_set_property(GDBusConnection *connection,
                    const gchar *sender,
                    const gchar *object_path,
                    const gchar *interface_name,
                    const gchar *property_name,
                    GVariant *value,
                    GError **error,
                    gpointer user_data)
{
    g_set_error(error,
                G_DBUS_ERROR,
                G_DBUS_ERROR_PROPERTY_READ_ONLY,
                "Property %s is read-only", property_name);
    return FALSE;
}

static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call,
    handle_get_property,
    handle_set_property
};

static void
initialize_server(GDBusConnection *connection)
{
    GError *error = NULL;
    server = g_new0(FlashlightServer, 1);
    server->connection = connection;
    server->brightness = 0;
    server->use_sysfs = g_file_test("/usr/lib/furios/device/flashlightd-sysfs", G_FILE_TEST_EXISTS);

    initialize_sysfs_paths(server);
    initialize_max_brightness(server);

    GDBusNodeInfo *introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (error != NULL) {
        g_critical("Error parsing introspection XML: %s", error->message);
        g_error_free(error);
        return;
    }

    server->registration_id = g_dbus_connection_register_object(connection,
                                                                FLASHLIGHTD_OBJECT_PATH,
                                                                introspection_data->interfaces[0],
                                                                &interface_vtable,
                                                                server,
                                                                NULL,
                                                                &error);

    g_dbus_node_info_unref(introspection_data);

    if (error != NULL) {
        g_critical("Error registering object: %s", error->message);
        g_error_free(error);
        return;
    }

    g_debug("Flashlightd service registered on D-Bus");
}

static void
on_bus_acquired(GDBusConnection *connection,
                const gchar *name,
                gpointer user_data)
{
    initialize_server(connection);
}

static void
on_name_lost(GDBusConnection *connection,
             const gchar *name,
             gpointer user_data)
{
    g_debug("Flashlightd D-Bus name lost");
    if (server != NULL) {
        cleanup_server(server);
        server = NULL;
    }
}

int
main(int argc, char *argv[])
{
    guint owner_id;
    GMainLoop *loop;

    g_debug("Starting flashlightd");

    gboolean force_sysfs = g_file_test("/usr/lib/furios/device/flashlightd-sysfs", G_FILE_TEST_EXISTS);
    if (force_sysfs)
        g_debug("Sysfs mode is forced by configuration file");

    if (!force_sysfs) {
        g_debug("Initializing GStreamer");
        gst_init(&argc, &argv);
    }

    g_debug("Registering on D-Bus as %s", FLASHLIGHTD_BUS_NAME);

    owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                              FLASHLIGHTD_BUS_NAME,
                              G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
                              on_bus_acquired,
                              NULL,
                              on_name_lost,
                              NULL,
                              NULL);

    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    g_bus_unown_name(owner_id);
    g_main_loop_unref(loop);

    if (server != NULL)
        cleanup_server(server);

    return 0;
}
