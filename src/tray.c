/*
 * StatusNotifierItem tray icon, implemented straight on GDBus.
 *
 * GTK4 dropped GtkStatusIcon and Fedora ships libayatana-appindicator only in
 * GTK2 and GTK3 flavours, neither of which can be linked into a GTK4 process.
 * The protocol underneath all of them is StatusNotifierItem over D-Bus, and
 * GNOME already runs a watcher for it (org.kde.StatusNotifierWatcher, provided
 * by the appindicator shell extension), so talking to it directly costs one
 * file and no new dependencies.
 *
 * Hosts expect a com.canonical.dbusmenu object alongside the item, so a small
 * three-entry menu is exported as well -- without it, right-clicking the icon
 * does nothing on some hosts.
 */
#include "tray.h"

#include <gio/gio.h>
#include <unistd.h>

#define SNI_WATCHER_NAME  "org.kde.StatusNotifierWatcher"
#define SNI_WATCHER_PATH  "/StatusNotifierWatcher"
#define SNI_ITEM_PATH     "/StatusNotifierItem"
#define SNI_MENU_PATH     "/MenuBar"

enum {
    MENU_ROOT = 0,
    MENU_OPEN = 1,
    MENU_SEPARATOR = 2,
    MENU_QUIT = 3,
};

struct WaTray {
    GDBusConnection *bus;
    char            *bus_name;
    char            *icon_name;
    char            *attention_icon;
    char            *title;
    char            *desktop_id;
    char            *launcher_path;
    guint            own_id;
    guint            item_id;
    guint            menu_id;
    WaTrayCallbacks  callbacks;
    gpointer         user_data;
    gboolean         needs_attention;
    int              unread;
};

static const char ITEM_XML[] =
    "<node>"
    "  <interface name='org.kde.StatusNotifierItem'>"
    "    <property name='Category' type='s' access='read'/>"
    "    <property name='Id' type='s' access='read'/>"
    "    <property name='Title' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='IconName' type='s' access='read'/>"
    "    <property name='AttentionIconName' type='s' access='read'/>"
    "    <property name='OverlayIconName' type='s' access='read'/>"
    "    <property name='ItemIsMenu' type='b' access='read'/>"
    "    <property name='Menu' type='o' access='read'/>"
    "    <method name='Activate'>"
    "      <arg name='x' type='i' direction='in'/>"
    "      <arg name='y' type='i' direction='in'/>"
    "    </method>"
    "    <method name='SecondaryActivate'>"
    "      <arg name='x' type='i' direction='in'/>"
    "      <arg name='y' type='i' direction='in'/>"
    "    </method>"
    "    <method name='ContextMenu'>"
    "      <arg name='x' type='i' direction='in'/>"
    "      <arg name='y' type='i' direction='in'/>"
    "    </method>"
    "    <method name='Scroll'>"
    "      <arg name='delta' type='i' direction='in'/>"
    "      <arg name='orientation' type='s' direction='in'/>"
    "    </method>"
    "    <signal name='NewIcon'/>"
    "    <signal name='NewTitle'/>"
    "    <signal name='NewStatus'><arg name='status' type='s'/></signal>"
    "  </interface>"
    "</node>";

static const char MENU_XML[] =
    "<node>"
    "  <interface name='com.canonical.dbusmenu'>"
    "    <property name='Version' type='u' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='TextDirection' type='s' access='read'/>"
    "    <property name='IconThemePath' type='as' access='read'/>"
    "    <method name='GetLayout'>"
    "      <arg name='parentId' type='i' direction='in'/>"
    "      <arg name='recursionDepth' type='i' direction='in'/>"
    "      <arg name='propertyNames' type='as' direction='in'/>"
    "      <arg name='revision' type='u' direction='out'/>"
    "      <arg name='layout' type='(ia{sv}av)' direction='out'/>"
    "    </method>"
    "    <method name='GetGroupProperties'>"
    "      <arg name='ids' type='ai' direction='in'/>"
    "      <arg name='propertyNames' type='as' direction='in'/>"
    "      <arg name='properties' type='a(ia{sv})' direction='out'/>"
    "    </method>"
    "    <method name='GetProperty'>"
    "      <arg name='id' type='i' direction='in'/>"
    "      <arg name='name' type='s' direction='in'/>"
    "      <arg name='value' type='v' direction='out'/>"
    "    </method>"
    "    <method name='Event'>"
    "      <arg name='id' type='i' direction='in'/>"
    "      <arg name='eventId' type='s' direction='in'/>"
    "      <arg name='data' type='v' direction='in'/>"
    "      <arg name='timestamp' type='u' direction='in'/>"
    "    </method>"
    "    <method name='AboutToShow'>"
    "      <arg name='id' type='i' direction='in'/>"
    "      <arg name='needUpdate' type='b' direction='out'/>"
    "    </method>"
    "    <signal name='ItemsPropertiesUpdated'>"
    "      <arg name='updatedProps' type='a(ia{sv})'/>"
    "      <arg name='removedProps' type='a(ias)'/>"
    "    </signal>"
    "    <signal name='LayoutUpdated'>"
    "      <arg name='revision' type='u'/>"
    "      <arg name='parent' type='i'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

/* ------------------------------------------------------------- item vtable */

static void
item_method_call(GDBusConnection *bus, const char *sender, const char *path,
                 const char *interface, const char *method, GVariant *params,
                 GDBusMethodInvocation *invocation, gpointer user_data)
{
    WaTray *tray = user_data;

    if (g_str_equal(method, "Activate") || g_str_equal(method, "SecondaryActivate")) {
        if (tray->callbacks.activate)
            tray->callbacks.activate(tray->user_data);
    }
    /* ContextMenu and Scroll need no action: the host renders the exported
     * dbusmenu itself. */
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *
item_get_property(GDBusConnection *bus, const char *sender, const char *path,
                  const char *interface, const char *name, GError **error,
                  gpointer user_data)
{
    WaTray *tray = user_data;

    if (g_str_equal(name, "Category"))          return g_variant_new_string("Communications");
    if (g_str_equal(name, "Id"))                return g_variant_new_string("whatsapp");
    if (g_str_equal(name, "Title"))             return g_variant_new_string(tray->title);
    if (g_str_equal(name, "Status"))
        return g_variant_new_string(tray->needs_attention ? "NeedsAttention" : "Active");
    /* Hosts vary on whether they honour AttentionIconName, so IconName follows
     * the state too -- that way the badge shows up either way, and since both
     * names are then equal it can never be drawn twice. */
    if (g_str_equal(name, "IconName"))
        return g_variant_new_string(tray->needs_attention ? tray->attention_icon : tray->icon_name);
    if (g_str_equal(name, "AttentionIconName")) return g_variant_new_string(tray->attention_icon);
    if (g_str_equal(name, "OverlayIconName"))   return g_variant_new_string("");
    /* FALSE means a left click should call Activate rather than pop the menu. */
    if (g_str_equal(name, "ItemIsMenu"))        return g_variant_new_boolean(FALSE);
    if (g_str_equal(name, "Menu"))              return g_variant_new_object_path(SNI_MENU_PATH);

    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "no property %s", name);
    return NULL;
}

static const GDBusInterfaceVTable ITEM_VTABLE = {
    item_method_call, item_get_property, NULL, { 0 }
};

/* ------------------------------------------------------------- menu vtable */

static GVariant *
menu_entry_properties(int id)
{
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

    switch (id) {
    case MENU_OPEN:
        g_variant_builder_add(&props, "{sv}", "label",   g_variant_new_string("Open WhatsApp"));
        g_variant_builder_add(&props, "{sv}", "enabled", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
        break;
    case MENU_SEPARATOR:
        g_variant_builder_add(&props, "{sv}", "type",    g_variant_new_string("separator"));
        g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
        break;
    case MENU_QUIT:
        g_variant_builder_add(&props, "{sv}", "label",   g_variant_new_string("Quit"));
        g_variant_builder_add(&props, "{sv}", "enabled", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
        break;
    default:
        g_variant_builder_add(&props, "{sv}", "children-display", g_variant_new_string("submenu"));
        break;
    }
    return g_variant_builder_end(&props);
}

static GVariant *
menu_entry_layout(int id)
{
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    return g_variant_new("(i@a{sv}@av)", id,
                         menu_entry_properties(id),
                         g_variant_builder_end(&children));
}

static void
menu_method_call(GDBusConnection *bus, const char *sender, const char *path,
                 const char *interface, const char *method, GVariant *params,
                 GDBusMethodInvocation *invocation, gpointer user_data)
{
    WaTray *tray = user_data;

    if (g_str_equal(method, "GetLayout")) {
        GVariantBuilder children;
        g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
        g_variant_builder_add(&children, "v", menu_entry_layout(MENU_OPEN));
        g_variant_builder_add(&children, "v", menu_entry_layout(MENU_SEPARATOR));
        g_variant_builder_add(&children, "v", menu_entry_layout(MENU_QUIT));

        GVariant *layout = g_variant_new("(i@a{sv}@av)", MENU_ROOT,
                                         menu_entry_properties(MENU_ROOT),
                                         g_variant_builder_end(&children));
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(u@(ia{sv}av))", 1u, layout));
        return;
    }

    if (g_str_equal(method, "GetGroupProperties")) {
        GVariantBuilder out;
        g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
        const int ids[] = { MENU_OPEN, MENU_SEPARATOR, MENU_QUIT };
        for (gsize i = 0; i < G_N_ELEMENTS(ids); i++)
            g_variant_builder_add(&out, "(i@a{sv})", ids[i], menu_entry_properties(ids[i]));
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(a(ia{sv}))", &out));
        return;
    }

    if (g_str_equal(method, "GetProperty")) {
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(v)", g_variant_new_string("")));
        return;
    }

    if (g_str_equal(method, "AboutToShow")) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        return;
    }

    if (g_str_equal(method, "Event")) {
        gint32 id = 0;
        const char *event_id = NULL;
        g_variant_get_child(params, 0, "i", &id);
        g_variant_get_child(params, 1, "&s", &event_id);

        if (g_strcmp0(event_id, "clicked") == 0) {
            if (id == MENU_OPEN && tray->callbacks.activate)
                tray->callbacks.activate(tray->user_data);
            else if (id == MENU_QUIT && tray->callbacks.quit)
                tray->callbacks.quit(tray->user_data);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *
menu_get_property(GDBusConnection *bus, const char *sender, const char *path,
                  const char *interface, const char *name, GError **error,
                  gpointer user_data)
{
    if (g_str_equal(name, "Version"))       return g_variant_new_uint32(3);
    if (g_str_equal(name, "Status"))        return g_variant_new_string("normal");
    if (g_str_equal(name, "TextDirection")) return g_variant_new_string("ltr");
    if (g_str_equal(name, "IconThemePath")) {
        GVariantBuilder paths;
        g_variant_builder_init(&paths, G_VARIANT_TYPE("as"));
        return g_variant_builder_end(&paths);
    }
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "no property %s", name);
    return NULL;
}

static const GDBusInterfaceVTable MENU_VTABLE = {
    menu_method_call, menu_get_property, NULL, { 0 }
};

/* ------------------------------------------------------------ registration */

static void
on_registered(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    if (!reply) {
        g_warning("tray: the watcher refused our item: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        return;
    }
    g_variant_unref(reply);
    g_message("tray: icon registered");
}

static void
on_name_acquired(GDBusConnection *bus, const char *name, gpointer user_data)
{
    WaTray *tray = user_data;

    g_dbus_connection_call(bus, SNI_WATCHER_NAME, SNI_WATCHER_PATH, SNI_WATCHER_NAME,
                           "RegisterStatusNotifierItem",
                           g_variant_new("(s)", tray->bus_name),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                           on_registered, tray);
}

/* The dock badge is a separate protocol from the tray icon: a broadcast
 * com.canonical.Unity.LauncherEntry signal, which Dash to Panel and Dash to Dock
 * both listen for. The object path carries a hash of the application URI, the
 * way libunity derives it -- hosts match on the interface, but keeping the
 * convention avoids surprising any that do look. */
static void
emit_launcher_badge(WaTray *tray, int count)
{
    if (!tray->launcher_path)
        return;

    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "count", g_variant_new_int64(count));
    g_variant_builder_add(&props, "{sv}", "count-visible", g_variant_new_boolean(count > 0));
    g_variant_builder_add(&props, "{sv}", "urgent", g_variant_new_boolean(count > 0));

    char *uri = g_strdup_printf("application://%s", tray->desktop_id);
    g_dbus_connection_emit_signal(tray->bus, NULL, tray->launcher_path,
                                  "com.canonical.Unity.LauncherEntry", "Update",
                                  g_variant_new("(s@a{sv})", uri,
                                                g_variant_builder_end(&props)),
                                  NULL);
    g_free(uri);
}

WaTray *
wa_tray_new(const char *icon_name, const char *title, const char *desktop_id,
            const WaTrayCallbacks *callbacks, gpointer user_data)
{
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!bus) {
        g_warning("tray: no session bus: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        return NULL;
    }

    WaTray *tray = g_new0(WaTray, 1);
    tray->bus       = bus;
    tray->icon_name      = g_strdup(icon_name);
    tray->attention_icon = g_strdup_printf("%s-attention", icon_name);
    tray->title          = g_strdup(title);
    tray->desktop_id     = g_strdup(desktop_id);

    char *uri = g_strdup_printf("application://%s", desktop_id);
    tray->launcher_path = g_strdup_printf("/com/canonical/unity/launcherentry/%u",
                                          g_str_hash(uri));
    g_free(uri);
    tray->callbacks = *callbacks;
    tray->user_data = user_data;
    /* The watcher keys items by bus name, and the convention is to include the
     * pid so two copies of an app do not collide. */
    tray->bus_name  = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", getpid());

    GDBusNodeInfo *item_node = g_dbus_node_info_new_for_xml(ITEM_XML, &error);
    GDBusNodeInfo *menu_node = g_dbus_node_info_new_for_xml(MENU_XML, &error);
    if (!item_node || !menu_node) {
        g_warning("tray: bad introspection XML: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        wa_tray_free(tray);
        return NULL;
    }

    tray->item_id = g_dbus_connection_register_object(bus, SNI_ITEM_PATH,
                                                      item_node->interfaces[0],
                                                      &ITEM_VTABLE, tray, NULL, &error);
    tray->menu_id = g_dbus_connection_register_object(bus, SNI_MENU_PATH,
                                                      menu_node->interfaces[0],
                                                      &MENU_VTABLE, tray, NULL, &error);
    g_dbus_node_info_unref(item_node);
    g_dbus_node_info_unref(menu_node);

    if (!tray->item_id || !tray->menu_id) {
        g_warning("tray: could not export the item: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        wa_tray_free(tray);
        return NULL;
    }

    tray->own_id = g_bus_own_name_on_connection(bus, tray->bus_name,
                                                G_BUS_NAME_OWNER_FLAGS_NONE,
                                                on_name_acquired, NULL, tray, NULL);
    return tray;
}

void
wa_tray_set_unread(WaTray *tray, int count)
{
    if (!tray || tray->unread == count)
        return;

    tray->unread = count;
    emit_launcher_badge(tray, count);

    const gboolean needs_attention = (count > 0);
    if (tray->needs_attention == needs_attention)
        return;

    tray->needs_attention = needs_attention;

    g_dbus_connection_emit_signal(tray->bus, NULL, SNI_ITEM_PATH,
                                  "org.kde.StatusNotifierItem", "NewStatus",
                                  g_variant_new("(s)",
                                                needs_attention ? "NeedsAttention" : "Active"),
                                  NULL);
    /* NewStatus alone leaves some hosts drawing the old pixmap. */
    g_dbus_connection_emit_signal(tray->bus, NULL, SNI_ITEM_PATH,
                                  "org.kde.StatusNotifierItem", "NewIcon", NULL, NULL);
    g_message("tray: %d unread", count);
}

void
wa_tray_free(WaTray *tray)
{
    if (!tray)
        return;
    if (tray->own_id)
        g_bus_unown_name(tray->own_id);
    if (tray->item_id)
        g_dbus_connection_unregister_object(tray->bus, tray->item_id);
    if (tray->menu_id)
        g_dbus_connection_unregister_object(tray->bus, tray->menu_id);
    g_clear_object(&tray->bus);
    g_free(tray->bus_name);
    g_free(tray->icon_name);
    g_free(tray->attention_icon);
    g_free(tray->desktop_id);
    g_free(tray->launcher_path);
    g_free(tray->title);
    g_free(tray);
}
