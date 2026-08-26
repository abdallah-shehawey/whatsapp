/*
 * wa-lite -- a small WhatsApp Web client for GTK4 + WebKitGTK 6.
 *
 * Two things it does differently from the packaged clients:
 *
 *   Paste. WebKitGTK hands the page an empty clipboardData for images: a real
 *   Ctrl+V fires `paste` with types=[] items=[] files=[], measured on
 *   webkit2gtk 2.52.5, so WhatsApp's handler finds nothing and drops it. Worse,
 *   WhatsApp's composer is plaintext-only, so WebKit does not even do its usual
 *   fallback of inserting an <img src="blob:"> that could be scraped back out.
 *   wa-lite therefore never asks WebKit for the clipboard at all: it reads the
 *   image on the GTK side, where the data is plainly available, and hands the
 *   bytes to the page itself.
 *
 *   Memory. WebKit's own pressure handler is switched on, so it sheds caches and
 *   runs GC as it approaches a ceiling. That is very different from capping the
 *   process with a cgroup, which can only evict pages blindly -- MemoryHigh=1100M
 *   on the packaged client drove memory.pressure to full avg10=63, i.e. frozen
 *   roughly two thirds of the time, while cpu.pressure stayed at zero.
 */
#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include "inject.js.h"
#include "tray.h"

#define WA_APP_ID     "io.github.shehawey.walite"
#define WA_ICON_NAME  "io.github.shehawey.walite"
#define WA_TRAY_ICON  "io.github.shehawey.walite-tray"
#define WA_TITLE      "WhatsApp"
#define WA_URL        "https://web.whatsapp.com/"

/* Presenting as desktop Chrome is what gets the full web client. Setting this
 * is not enough on its own: WebKitGTK ships site-specific quirks that rewrite
 * the user agent for a list of hosts including whatsapp.com, and the quirk wins
 * over anything the application sets -- which is why the page was reporting
 * "Macintosh; Intel Mac OS X" and offering a Mac download. See build_window,
 * where the quirks are switched off. */
#define WA_USER_AGENT \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) " \
    "Chrome/140.0.0.0 Safari/537.36"

/* Ceiling for WebKit's in-process pressure handler, in MiB. It starts shedding
 * caches at the conservative threshold and gets aggressive at the strict one. */
#define WA_MEMORY_LIMIT_MB     1600
#define WA_CONSERVATIVE_FRAC   0.40
#define WA_STRICT_FRAC         0.70
#define WA_POLL_INTERVAL_SECS  10.0

#define WA_DEFAULT_WIDTH   1200
#define WA_DEFAULT_HEIGHT   800

typedef struct {
    GtkApplication *app;
    GtkWindow      *window;
    WebKitWebView  *view;
    WaTray         *tray;
    char           *config_path;
    gboolean        start_hidden;
} WaApp;

/* ------------------------------------------------------------------ config */

static void
config_load(WaApp *self, int *width, int *height, double *zoom)
{
    *width  = WA_DEFAULT_WIDTH;
    *height = WA_DEFAULT_HEIGHT;
    *zoom   = 1.0;

    GKeyFile *keys = g_key_file_new();
    if (g_key_file_load_from_file(keys, self->config_path, G_KEY_FILE_NONE, NULL)) {
        int w = (int)g_key_file_get_integer(keys, "window", "width", NULL);
        int h = (int)g_key_file_get_integer(keys, "window", "height", NULL);
        double z = g_key_file_get_double(keys, "view", "zoom", NULL);
        if (w > 400)  *width  = w;
        if (h > 300)  *height = h;
        if (z > 0.25) *zoom   = z;
    }
    g_key_file_free(keys);
}

static void
config_save(WaApp *self)
{
    if (!self->window || !self->view)
        return;

    GKeyFile *keys = g_key_file_new();
    g_key_file_load_from_file(keys, self->config_path, G_KEY_FILE_NONE, NULL);
    g_key_file_set_integer(keys, "window", "width",  gtk_widget_get_width(GTK_WIDGET(self->window)));
    g_key_file_set_integer(keys, "window", "height", gtk_widget_get_height(GTK_WIDGET(self->window)));
    g_key_file_set_double(keys, "view", "zoom", webkit_web_view_get_zoom_level(self->view));

    char *dir = g_path_get_dirname(self->config_path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    g_key_file_save_to_file(keys, self->config_path, NULL);
    g_key_file_free(keys);
}

/* -------------------------------------------------------------- appearance */

#define GNOME_INTERFACE_SCHEMA "org.gnome.desktop.interface"

/* Looked up rather than constructed blindly: g_settings_new aborts the process
 * if the schema is missing, which would take out anyone not running GNOME. */
static GSettings *
interface_settings(void)
{
    static GSettings *settings;
    static gboolean   resolved;

    if (!resolved) {
        resolved = TRUE;
        GSettingsSchema *schema = g_settings_schema_source_lookup(
            g_settings_schema_source_get_default(), GNOME_INTERFACE_SCHEMA, TRUE);
        if (schema) {
            settings = g_settings_new(GNOME_INTERFACE_SCHEMA);
            g_settings_schema_unref(schema);
        }
    }
    return settings;
}

/* WebKit derives prefers-color-scheme from GTK's dark preference, and WhatsApp
 * Web keys its own theme off that media query -- so setting this is what
 * actually turns the chat dark. Only an explicit "prefer-light" opts out. */
static void
apply_color_scheme(void)
{
    gboolean dark = TRUE;
    GSettings *settings = interface_settings();

    if (settings) {
        char *scheme = g_settings_get_string(settings, "color-scheme");
        dark = (g_strcmp0(scheme, "prefer-light") != 0);
        g_free(scheme);
    }
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", dark, NULL);
}

static void
on_color_scheme_changed(GSettings *settings, const char *key, gpointer user_data)
{
    apply_color_scheme();
}

/* An explicit [view] font in the config wins so it can be changed without a
 * rebuild; otherwise follow whatever GNOME uses for the rest of the desktop. */
static char *
resolve_font(WaApp *self)
{
    GKeyFile *keys = g_key_file_new();
    char *font = NULL;
    if (g_key_file_load_from_file(keys, self->config_path, G_KEY_FILE_NONE, NULL))
        font = g_key_file_get_string(keys, "view", "font", NULL);
    g_key_file_free(keys);

    if (font && *font)
        return font;
    g_free(font);

    GSettings *settings = interface_settings();
    return settings ? g_settings_get_string(settings, "font-name")
                    : g_strdup("Cantarell 11");
}

/* WhatsApp Web names its own font stack in CSS, so setting WebKit's defaults is
 * not enough to change what is actually drawn -- a user-level sheet is. The
 * fallbacks matter: a display face like PoetsenOne carries no Arabic glyphs, and
 * fontconfig only substitutes per-glyph if something further down the list has
 * them. */
static void
apply_font(WebKitSettings *settings, WebKitUserContentManager *content, const char *font_spec)
{
    PangoFontDescription *desc = pango_font_description_from_string(font_spec);
    const char *family = pango_font_description_get_family(desc);
    int points = pango_font_description_get_size(desc) / PANGO_SCALE;

    if (!family || !*family)
        family = "sans-serif";
    if (points <= 0)
        points = 11;

    /* GNOME states sizes in points; WebKit wants CSS pixels at 96 dpi. */
    const int pixels = (int)(points * 96.0 / 72.0 + 0.5);

    webkit_settings_set_default_font_family(settings, family);
    webkit_settings_set_sans_serif_font_family(settings, family);
    webkit_settings_set_default_font_size(settings, (guint32)pixels);

    char *css = g_strdup_printf(
        "* { font-family: \"%s\", system-ui, sans-serif !important; }", family);
    WebKitUserStyleSheet *sheet = webkit_user_style_sheet_new(
        css, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
    webkit_user_content_manager_add_style_sheet(content, sheet);
    webkit_user_style_sheet_unref(sheet);

    g_message("font: %s at %dpx", family, pixels);
    g_free(css);
    pango_font_description_free(desc);
}

/* --------------------------------------------------------------- clipboard */

/* Formats a screenshot lands on the clipboard as, best first. */
static const char *WA_IMAGE_MIMES[] = {
    "image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp", NULL
};

typedef struct {
    WaApp        *app;
    char         *mime;
    GOutputStream *sink;
} PasteCtx;

static void
paste_ctx_free(PasteCtx *ctx)
{
    g_clear_object(&ctx->sink);
    g_free(ctx->mime);
    g_free(ctx);
}

static gboolean
clipboard_has_image(GtkWidget *widget)
{
    GdkClipboard *clipboard = gtk_widget_get_clipboard(widget);
    GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
    if (!formats)
        return FALSE;

    for (int i = 0; WA_IMAGE_MIMES[i]; i++)
        if (gdk_content_formats_contain_mime_type(formats, WA_IMAGE_MIMES[i]))
            return TRUE;

    return gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE);
}

/* Intercept Ctrl+V only for a clipboard that holds an image and no text.
 * Rich-text copies often carry both, and there the user means the text -- the
 * page-side route below picks up anything this declines. */
static gboolean
clipboard_holds_image_only(GtkWidget *widget)
{
    GdkClipboard *clipboard = gtk_widget_get_clipboard(widget);
    GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
    if (!formats) {
        g_message("paste: the clipboard offers nothing");
        return FALSE;
    }

    char *offered = gdk_content_formats_to_string(formats);
    g_message("paste: clipboard offers %s", offered);
    g_free(offered);

    if (gdk_content_formats_contain_mime_type(formats, "text/plain") ||
        gdk_content_formats_contain_mime_type(formats, "text/plain;charset=utf-8")) {
        g_message("paste: text present, leaving it to WebKit");
        return FALSE;
    }

    return clipboard_has_image(widget);
}

static void
deliver_image_to_page(WaApp *self, const guchar *data, gsize length, const char *mime)
{
    char *encoded = g_base64_encode(data, length);

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", "b64",  g_variant_new_string(encoded));
    g_variant_builder_add(&builder, "{sv}", "mime", g_variant_new_string(mime));

    webkit_web_view_call_async_javascript_function(
        self->view, "return window.__waLitePasteImage(b64, mime);", -1,
        g_variant_builder_end(&builder), NULL, NULL, NULL, NULL, NULL);

    g_message("pasted %" G_GSIZE_FORMAT " bytes of %s into the page", length, mime);
    g_free(encoded);
}

static void
on_splice_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    PasteCtx *ctx = user_data;
    GError *error = NULL;

    if (g_output_stream_splice_finish(G_OUTPUT_STREAM(source), result, &error) < 0) {
        g_warning("could not read the clipboard image: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        paste_ctx_free(ctx);
        return;
    }

    GBytes *bytes = g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(ctx->sink));
    gsize length = 0;
    const guchar *data = g_bytes_get_data(bytes, &length);
    if (length > 0)
        deliver_image_to_page(ctx->app, data, length, ctx->mime);
    else
        g_warning("clipboard image was empty");

    g_bytes_unref(bytes);
    paste_ctx_free(ctx);
}

/* Fall back to the texture representation when no raw image MIME type is on
 * offer; this re-encodes rather than copying the original bytes. */
static void
on_texture_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    WaApp *self = user_data;
    GError *error = NULL;

    GdkTexture *texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source), result, &error);
    if (!texture) {
        g_warning("no usable image on the clipboard: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        return;
    }

    GBytes *png = gdk_texture_save_to_png_bytes(texture);
    g_object_unref(texture);
    if (!png) {
        g_warning("could not encode the clipboard image as PNG");
        return;
    }

    gsize length = 0;
    const guchar *data = g_bytes_get_data(png, &length);
    deliver_image_to_page(self, data, length, "image/png");
    g_bytes_unref(png);
}

static void
on_clipboard_stream_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    WaApp *self = user_data;
    GdkClipboard *clipboard = GDK_CLIPBOARD(source);
    const char *mime = NULL;
    GError *error = NULL;

    GInputStream *stream = gdk_clipboard_read_finish(clipboard, result, &mime, &error);
    if (!stream) {
        g_clear_error(&error);
        gdk_clipboard_read_texture_async(clipboard, NULL, on_texture_ready, self);
        return;
    }

    PasteCtx *ctx = g_new0(PasteCtx, 1);
    ctx->app  = self;
    ctx->mime = g_strdup(mime ? mime : "image/png");
    ctx->sink = g_memory_output_stream_new_resizable();

    g_output_stream_splice_async(ctx->sink, stream,
                                 G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                 G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                 G_PRIORITY_DEFAULT, NULL, on_splice_done, ctx);
    g_object_unref(stream);
}

static void
paste_clipboard_image(WaApp *self)
{
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self->window));
    gdk_clipboard_read_async(clipboard, (const char **)WA_IMAGE_MIMES, G_PRIORITY_DEFAULT, NULL,
                             on_clipboard_stream_ready, self);
}

/* ------------------------------------------------------------------- input */

static void
adjust_zoom(WaApp *self, double delta, gboolean reset)
{
    double zoom = reset ? 1.0 : webkit_web_view_get_zoom_level(self->view) + delta;
    zoom = CLAMP(zoom, 0.5, 3.0);
    webkit_web_view_set_zoom_level(self->view, zoom);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, gpointer user_data)
{
    WaApp *self = user_data;

    if (!(state & GDK_CONTROL_MASK))
        return GDK_EVENT_PROPAGATE;

    switch (keyval) {
    case GDK_KEY_v:
    case GDK_KEY_V:
        g_message("paste: Ctrl+V intercepted");
        /* Let text paste take WebKit's native path, which works fine. */
        if (!clipboard_holds_image_only(GTK_WIDGET(self->window)))
            return GDK_EVENT_PROPAGATE;
        paste_clipboard_image(self);
        return GDK_EVENT_STOP;

    case GDK_KEY_plus:
    case GDK_KEY_equal:
    case GDK_KEY_KP_Add:
        adjust_zoom(self, 0.1, FALSE);
        return GDK_EVENT_STOP;

    case GDK_KEY_minus:
    case GDK_KEY_KP_Subtract:
        adjust_zoom(self, -0.1, FALSE);
        return GDK_EVENT_STOP;

    case GDK_KEY_0:
    case GDK_KEY_KP_0:
        adjust_zoom(self, 0.0, TRUE);
        return GDK_EVENT_STOP;

    case GDK_KEY_q:
    case GDK_KEY_Q:
        config_save(self);
        g_application_quit(G_APPLICATION(self->app));
        return GDK_EVENT_STOP;

    default:
        return GDK_EVENT_PROPAGATE;
    }
}

/* ------------------------------------------------------------- web plumbing */

/* Second route into the paste path. The key controller can miss Ctrl+V -- the
 * WebView may consume the key first -- so inject.js also reports any paste that
 * arrives with an empty clipboardData, and we serve it from the GTK clipboard.
 * The two cannot double up: when the key controller handles a paste it stops the
 * event, so WebKit never fires one for the page to report. */
static void
on_paste_request(WebKitUserContentManager *manager, JSCValue *value, gpointer user_data)
{
    WaApp *self = user_data;

    if (!clipboard_has_image(GTK_WIDGET(self->window))) {
        g_message("paste: page asked for an image, clipboard has none");
        return;
    }
    g_message("paste: serving the page from the GTK clipboard");
    paste_clipboard_image(self);
}

static void
on_script_message(WebKitUserContentManager *manager, JSCValue *value, gpointer user_data)
{
    char *text = jsc_value_to_string(value);
    g_message("[page] %s", text);
    g_free(text);
}

/* Grant only what WhatsApp actually needs: notifications, and the microphone
 * and camera for voice notes and calls. Everything else is refused. */
static gboolean
on_permission_request(WebKitWebView *view, WebKitPermissionRequest *request, gpointer user_data)
{
    if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(request) ||
        WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request))
        webkit_permission_request_allow(request);
    else
        webkit_permission_request_deny(request);
    return TRUE;
}

static gboolean
on_decide_destination(WebKitDownload *download, gchar *suggested_filename, gpointer user_data)
{
    const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (!dir)
        dir = g_get_home_dir();

    char *path = g_build_filename(dir, suggested_filename && *suggested_filename
                                       ? suggested_filename : "whatsapp-download", NULL);
    webkit_download_set_destination(download, path);
    g_message("saving download to %s", path);
    g_free(path);
    return TRUE;
}

static void
on_download_started(WebKitNetworkSession *session, WebKitDownload *download, gpointer user_data)
{
    g_signal_connect(download, "decide-destination", G_CALLBACK(on_decide_destination), NULL);
}

/* WebKit's pressure handler is process-wide and has to be set before any
 * network session exists. */
static void
configure_memory_pressure(void)
{
    WebKitMemoryPressureSettings *pressure = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(pressure, WA_MEMORY_LIMIT_MB);
    webkit_memory_pressure_settings_set_conservative_threshold(pressure, WA_CONSERVATIVE_FRAC);
    webkit_memory_pressure_settings_set_strict_threshold(pressure, WA_STRICT_FRAC);
    /* Zero disables the kill threshold: shedding caches is fine, losing the
     * window because a chat had too many photos in it is not. */
    webkit_memory_pressure_settings_set_kill_threshold(pressure, 0.0);
    webkit_memory_pressure_settings_set_poll_interval(pressure, WA_POLL_INTERVAL_SECS);

    webkit_network_session_set_memory_pressure_settings(pressure);
    webkit_memory_pressure_settings_free(pressure);
}

/* WhatsApp Web puts "(3) WhatsApp" in the document title while chats are unread
 * and drops the prefix once they are read. That is the only unread signal the
 * page hands us without scraping its DOM, and it is what drives the tray badge. */
static void
on_title_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    WaApp *self = user_data;
    const char *title = webkit_web_view_get_title(self->view);

    wa_tray_set_attention(self->tray, title && title[0] == '(');

    if (self->window)
        gtk_window_set_title(self->window, (title && *title) ? title : WA_TITLE);
}

/* ------------------------------------------------------------------ window */

static gboolean
on_close_request(GtkWindow *window, gpointer user_data)
{
    WaApp *self = user_data;
    config_save(self);
    /* Stay resident so notifications keep arriving; relaunching re-shows us. */
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    return TRUE;
}

static void
build_window(WaApp *self)
{
    int width, height;
    double zoom;
    config_load(self, &width, &height, &zoom);

    apply_color_scheme();
    GSettings *interface = interface_settings();
    if (interface)
        g_signal_connect(interface, "changed::color-scheme",
                         G_CALLBACK(on_color_scheme_changed), NULL);

    char *data_dir  = g_build_filename(g_get_user_data_dir(),  "wa-lite", NULL);
    char *cache_dir = g_build_filename(g_get_user_cache_dir(), "wa-lite", NULL);
    WebKitNetworkSession *session = webkit_network_session_new(data_dir, cache_dir);
    g_signal_connect(session, "download-started", G_CALLBACK(on_download_started), self);
    g_free(data_dir);
    g_free(cache_dir);

    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_user_agent(settings, WA_USER_AGENT);
    /* Without this the quirk for whatsapp.com overrides the line above and the
     * page sees Safari on macOS. */
    webkit_settings_set_enable_site_specific_quirks(settings, FALSE);
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    webkit_settings_set_javascript_can_access_clipboard(settings, TRUE);
    /* A single-page app never navigates back, so the page cache is pure
     * overhead; WebGL is not used by WhatsApp Web at all. */
    webkit_settings_set_enable_page_cache(settings, FALSE);
    webkit_settings_set_enable_back_forward_navigation_gestures(settings, FALSE);
    webkit_settings_set_enable_webgl(settings, FALSE);
    webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);

    WebKitUserContentManager *content = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(content, "walite", NULL);
    g_signal_connect(content, "script-message-received::walite",
                     G_CALLBACK(on_script_message), self);
    webkit_user_content_manager_register_script_message_handler(content, "walitePaste", NULL);
    g_signal_connect(content, "script-message-received::walitePaste",
                     G_CALLBACK(on_paste_request), self);

    char *font_spec = resolve_font(self);
    apply_font(settings, content, font_spec);
    g_free(font_spec);

    WebKitUserScript *script = webkit_user_script_new(
        WA_INJECT_JS,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        NULL, NULL);
    webkit_user_content_manager_add_script(content, script);
    webkit_user_script_unref(script);

    self->view = g_object_new(WEBKIT_TYPE_WEB_VIEW,
                              "network-session",      session,
                              "settings",             settings,
                              "user-content-manager", content,
                              NULL);
    g_object_unref(settings);
    g_object_unref(content);
    g_object_unref(session);

    webkit_web_view_set_settings(self->view, settings);

    /* Caches a moderate amount instead of the browser-sized default. */
    webkit_web_context_set_cache_model(webkit_web_view_get_context(self->view),
                                       WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);

    g_message("user agent: %s",
              webkit_settings_get_user_agent(webkit_web_view_get_settings(self->view)));

    g_signal_connect(self->view, "permission-request",
                     G_CALLBACK(on_permission_request), self);
    g_signal_connect(self->view, "notify::title",
                     G_CALLBACK(on_title_changed), self);
    webkit_web_view_set_zoom_level(self->view, zoom);

    self->window = GTK_WINDOW(gtk_application_window_new(self->app));
    gtk_window_set_title(self->window, WA_TITLE);
    gtk_window_set_icon_name(self->window, WA_ICON_NAME);
    gtk_window_set_default_size(self->window, width, height);
    gtk_window_set_child(self->window, GTK_WIDGET(self->view));
    g_signal_connect(self->window, "close-request", G_CALLBACK(on_close_request), self);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self->window), keys);

    webkit_web_view_load_uri(self->view, WA_URL);
}

/* --------------------------------------------------------------- lifecycle */

static void
show_window(WaApp *self)
{
    if (!self->window)
        return;
    gtk_widget_set_visible(GTK_WIDGET(self->window), TRUE);
    gtk_window_present(self->window);
}

static void
on_tray_activate(gpointer user_data)
{
    show_window(user_data);
}

static void
on_tray_quit(gpointer user_data)
{
    WaApp *self = user_data;
    config_save(self);
    g_application_quit(G_APPLICATION(self->app));
}

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    WaApp *self = user_data;
    const gboolean first_run = (self->window == NULL);

    if (first_run) {
        build_window(self);

        static const WaTrayCallbacks callbacks = { on_tray_activate, on_tray_quit };
        self->tray = wa_tray_new(WA_TRAY_ICON, WA_TITLE, &callbacks, self);

        /* Without a held reference GtkApplication would exit as soon as the
         * window is hidden, taking the tray icon and notifications with it. */
        g_application_hold(G_APPLICATION(app));
    }

    /* Autostart hands us --hidden so login does not throw a window in the
     * user's face; the session still connects and notifications still arrive.
     * Any later activation -- the launcher, or the tray -- shows the window. */
    if (first_run && self->start_hidden) {
        g_message("started in the background, window hidden");
        return;
    }

    show_window(self);
}

int
main(int argc, char **argv)
{
    configure_memory_pressure();

    WaApp self = { 0 };

    /* Consume --hidden here rather than registering it with GApplication: the
     * flag only ever matters to the primary instance, and stripping it keeps
     * option parsing from rejecting argv. */
    int kept = 1;
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--hidden") == 0)
            self.start_hidden = TRUE;
        else
            argv[kept++] = argv[i];
    }
    argc = kept;
    argv[argc] = NULL;

    g_set_application_name(WA_TITLE);

    self.config_path = g_build_filename(g_get_user_config_dir(), "wa-lite", "wa-lite.conf", NULL);
    self.app = gtk_application_new(WA_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(self.app, "activate", G_CALLBACK(on_activate), &self);

    int status = g_application_run(G_APPLICATION(self.app), argc, argv);

    wa_tray_free(self.tray);
    g_object_unref(self.app);
    g_free(self.config_path);
    return status;
}
