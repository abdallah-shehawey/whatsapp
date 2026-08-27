/*
 * whatsapp -- a small WhatsApp Web client for GTK4 + WebKitGTK 6.
 *
 * Two things it does differently from the packaged clients:
 *
 *   Paste. WebKitGTK hands the page an empty clipboardData for images: a real
 *   Ctrl+V fires `paste` with types=[] items=[] files=[], measured on
 *   webkit2gtk 2.52.5, so WhatsApp's handler finds nothing and drops it. Worse,
 *   WhatsApp's composer is plaintext-only, so WebKit does not even do its usual
 *   fallback of inserting an <img src="blob:"> that could be scraped back out.
 *   whatsapp therefore never asks WebKit for the clipboard at all: it reads the
 *   image on the GTK side, where the data is plainly available, and hands the
 *   bytes to the page itself.
 *
 *   Memory. WebKit's own pressure handler is switched on, so it sheds caches and
 *   runs GC as it approaches a ceiling. That is very different from capping the
 *   process with a cgroup, which can only evict pages blindly -- MemoryHigh=1100M
 *   on the packaged client drove memory.pressure to full avg10=63, i.e. frozen
 *   roughly two thirds of the time, while cpu.pressure stayed at zero.
 */
#include <stdlib.h>

#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include "inject.js.h"
#include "tray.h"

#define WA_APP_ID     "io.github.shehawey.whatsapp"
#define WA_ICON_NAME  "io.github.shehawey.whatsapp"
#define WA_TRAY_ICON  "io.github.shehawey.whatsapp-tray"
#define WA_TITLE      "WhatsApp"
#define WA_VERSION    "1.0.5"
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
 * caches at the conservative threshold and gets aggressive at the strict one.
 *
 * Measured, not guessed: a signed-in web process sits at ~1330 MB, so the old
 * 1600 MB ceiling put the strict threshold (70% = 1120 MB) permanently below the
 * working set. WebKit then shed caches without pause, and the visible symptom was
 * emoji sprite sheets being dropped and re-fetched -- "the emoji are broken when
 * I open WhatsApp". The ceiling has to sit above the working set for the handler
 * to do what it is for: catching runaway growth, not fighting normal use. */
#define WA_MEMORY_LIMIT_MB     2560
#define WA_CONSERVATIVE_FRAC   0.55
#define WA_STRICT_FRAC         0.85
#define WA_POLL_INTERVAL_SECS  10.0

#define WA_DEFAULT_WIDTH   1200
#define WA_DEFAULT_HEIGHT   800

/* How many banners are tracked at once. The click handler has to recognise the
 * ids still on screen, and messages are answered per conversation, so this is
 * also how many chats can hold a banner of their own. */
#define WA_BANNERS 8
/* How long a banner id stays reusable. The shell destroys a transient
 * notification the moment its banner hides, and an id it has forgotten simply
 * makes a new banner, so this is housekeeping rather than a guard. */
#define WA_BANNER_REUSE_US   (60000 * 1000)
/* How long a banner may stay on screen before the client takes it down itself
 * and posts the message again quietly. GNOME will not take it down on its own;
 * banner_expire is where that story is told. */
#define WA_BANNER_LIFETIME_S 12
/* How many notification ids stay clickable. A banner is gone in seconds, but the
 * entry it leaves in the notification centre can be clicked an hour later. */
#define WA_CLICKABLE 16
/* Notification pictures are written here, one file per distinct face. */
#define WA_AVATAR_PREFIX "whatsapp-avatar-"
/* Two reports of the same message from two different places -- the chat list
 * watcher and the document title, say -- inside this window are one message.
 * Two reports from the SAME place are two messages: somebody sending "tamam"
 * twice in a row deserves two banners, and swallowing the second was a bug. */
#define WA_NOTIFY_DEDUPE_US  (2500 * 1000)
/* The document title is only consulted for a message the chat list missed, so it
 * waits this long for the watcher to speak first. */
#define WA_TITLE_FALLBACK_US (2500 * 1000)
/* Chats that were already unread when the client started are not news. WhatsApp
 * spends the best part of half a minute syncing them, rewriting row after row
 * with messages that arrived while the client was not running, and the title
 * climbs from nothing to the standing count as it does. Measured on a live
 * launch: four conversations were announced fifteen seconds in, which is where
 * this number comes from. The page-side watcher also refuses anything whose
 * clock is not the current one, so this is the belt to that pair of braces. */
#define WA_STARTUP_GRACE_US  (30000 * 1000)

/* Where a notification came from. All the dedupe below needs to know is whether
 * two reports of one message came from two different places. */
typedef enum {
    WA_SOURCE_PAGE,     /* WhatsApp Web raised it: the window is not in front */
    WA_SOURCE_WATCHER,  /* the chat list watcher saw a message land */
    WA_SOURCE_TITLE,    /* the unread count moved and the watcher had nothing */
} WaSource;

/* One live banner. Messages from the same conversation replace each other
 * rather than queueing behind each other: GNOME shows one banner at a time,
 * holds three in its queue and drops everything past that on the floor, so a
 * burst of ten messages meant three banners spread over twelve seconds and
 * seven that were never seen at all. A replacement is not a quiet one -- the
 * shell clears the notification's `acknowledged` flag on every update, which
 * puts the banner back on screen and rings the sound again. */
typedef struct {
    char               *chat;   /* who it is from: the key messages replace on */
    char               *body;   /* what it said, for the quiet copy afterwards */
    char               *image;  /* and the face that went with it */
    guint32             id;     /* the id the daemon gave it, 0 while in flight */
    gint64              at;     /* when it was raised */
    guint               expiry; /* the timer that takes it down; see banner_expire */
    WebKitNotification *page;   /* the page's own notification, when it is one */
} WaBanner;

/* A notification the user can still click, banner or notification-centre entry. */
typedef struct {
    guint32             id;
    WebKitNotification *page;
} WaClickable;

/* Which banner a lifetime timer belongs to. The id rides along because the slot
 * may have moved on to a newer message by the time it fires. */
typedef struct {
    struct WaAppTag *app;
    int              slot;
    guint32          id;
} WaBannerExpiry;

typedef struct WaAppTag {
    GtkApplication *app;
    GtkWindow      *window;
    WebKitWebView  *view;
    WaTray         *tray;
    char           *config_path;
    GFileMonitor   *debug_monitor;
    int             unread_chats;
    WaBanner        banners[WA_BANNERS];
    int             banner_next;
    WaClickable     clickable[WA_CLICKABLE];
    int             clickable_next;
    gboolean        notify_subscribed;
    /* Whether the notification daemon rings for a sound-name hint. GNOME's does
     * and says so in GetCapabilities; one that does not is rung for here. */
    gboolean        notify_has_sound;
    /* What was last announced, from where, and when. Two places can report the
     * same message, and this is what keeps that from showing up as two banners. */
    char           *last_summary;
    char           *last_body;
    WaSource        last_source;
    gint64          last_notify_at;
    gint64          last_arrival_at;
    gint64          loaded_at;
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

/* WhatsApp Web sizes almost everything in rem, so the root font size scales the
 * entire client -- message text, the composer, chat names. It is deliberately
 * NOT taken from GNOME's interface font: that font is stated in points for a
 * desktop widget, and feeding it in here made the root 13px against the 16px
 * the web assumes, shrinking every message to 81% of its intended size. */
#define WA_DEFAULT_FONT_PX  16

static int
resolve_font_size(WaApp *self)
{
    GKeyFile *keys = g_key_file_new();
    int size = 0;
    if (g_key_file_load_from_file(keys, self->config_path, G_KEY_FILE_NONE, NULL))
        size = g_key_file_get_integer(keys, "view", "font-size", NULL);
    g_key_file_free(keys);

    return (size >= 10 && size <= 32) ? size : WA_DEFAULT_FONT_PX;
}

/* WhatsApp Web names its own font stack in CSS, so setting WebKit's defaults is
 * not enough to change what is actually drawn -- a user-level sheet is. The
 * fallbacks matter: a display face like PoetsenOne carries no Arabic glyphs, and
 * fontconfig only substitutes per-glyph if something further down the list has
 * them.
 *
 * One family, everywhere. Message text briefly had a separate reading face, on
 * the theory that a display font is a poor choice for a chat -- but the browser
 * this client is meant to be indistinguishable from is configured to ignore page
 * fonts entirely, so it draws bubbles, previews and controls in the desktop font
 * alike. Splitting them here was the only visible difference left between the
 * two, so the split is gone. */
static void
apply_font(WebKitSettings *settings, WebKitUserContentManager *content,
           const char *font_spec, int pixels)
{
    PangoFontDescription *desc = pango_font_description_from_string(font_spec);
    const char *family = pango_font_description_get_family(desc);

    if (!family || !*family)
        family = "sans-serif";

    webkit_settings_set_default_font_family(settings, family);
    webkit_settings_set_sans_serif_font_family(settings, family);
    webkit_settings_set_default_font_size(settings, (guint32)pixels);

    /* Three things this sheet has to get right, because it is !important on every
     * element and so whatever it leaves out is gone from the page:
     *
     *   Emoji families, or any emoji WhatsApp draws as text rather than as a
     *   sprite lands on a face with no glyph for it and renders as a blank box.
     *
     *   Arabic, which the desktop font here (PoetsenOne) does not carry a single
     *   glyph of. Naming a face is a courtesy rather than a control: WebKitGTK
     *   resolves a character its first family lacks through fontconfig and never
     *   looks at the rest of the list, which was measured by asking for Naskh and
     *   for Kufi and getting Noto Sans Arabic both times.
     *
     *   No line-height on the composer. Forcing one there was a mistake worth
     *   recording: WhatsApp already sets 1.47em on it, and overriding that with
     *   1.5 made the content one pixel taller than the box it sits in. A box with
     *   one pixel of overflow is a scrollable box, so every keystroke scrolled the
     *   caret back into view and the text twitched up and down -- which is exactly
     *   the symptom the override was added to fix. Measured: scrollHeight exceeded
     *   clientHeight by 1px on a three-line message, and zero without it. That is
     *   why the rule below stops at [contenteditable].
     *
     * The rules after the font stack are about Arabic, and about a line box
     * being too short rather than a font being wrong. WhatsApp gives its list and
     * its bubbles line boxes of about 1.43em and clips them with overflow:hidden.
     * The face WebKit falls back to for Arabic reserves 1.37em above the
     * baseline on its own, so the bowl of a final ن or ي -- which hangs well
     * below it -- was cut off, and the hook at the left end of each bowl was all
     * that survived. That left a stray comma after every second word: "يعني"
     * came out as "يعن ،". Nothing about it is the font's doing -- WhatsApp's own
     * font stack clips identically, and so does every Arabic face installed here
     * -- and no @font-face descriptor helps, because WebKitGTK ignores both
     * local() sources and ascent-override. A taller line box is the one thing
     * that fixes it. Measured on a 14px preview: clipped at 20px, 22px and 23.8px,
     * clean at 24px, i.e. anything at or above about 1.6em. Text elements are
     * named one by one rather than styling everything, so the surrounding layout
     * keeps the heights WhatsApp gave it.
     *
     * A version that left WhatsApp's line boxes alone and widened the CLIP
     * instead -- padding-bottom with a matching negative margin on the elements
     * that carry overflow:hidden -- shipped once and came straight back out. It
     * can only reach boxes that can be named, and in the chat list that is
     * div:has(> span[title]); a message bubble has no span[title] at all, so
     * every Arabic bubble went back to being shorn while the list changed shape.
     * The verdict on it was "you ruined the text, the one before was better", and
     * the one before is what is written here. */
    char *css = g_strdup_printf(
        "* { font-family: \"%s\", system-ui, \"Noto Sans Arabic\", \"Noto Color Emoji\", "
        "\"Apple Color Emoji\", \"Segoe UI Emoji\", sans-serif !important; }"
        /* Chat names, previews and message text -- every place a message is read.
           The composer is left alone on purpose; see above. */
        "span[title]:not([contenteditable] *),"
        "span[title]:not([contenteditable] *) *,"
        "span[dir]:not([contenteditable] *),"
        "span[dir]:not([contenteditable] *) * { line-height: 1.7 !important; }"
        /* The composer clips the same way and cannot be given a taller line box,
           so it is given a taller clip instead: padding grows the box that
           overflow:hidden cuts against, and the negative margin hands the space
           straight back to the layout. The line box is untouched, which is what
           keeps the caret from twitching. Measured after: scrollHeight equals
           clientHeight, where a line-height override left it one pixel over. */
        "[contenteditable=\"true\"]"
        "{ padding-bottom: 0.35em !important; margin-bottom: -0.35em !important; }",
        family);
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
        self->view, "return window.__whatsappPasteImage(b64, mime);", -1,
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

/* ------------------------------------------------------------------- focus */

/* WhatsApp Web decides two things from document.hasFocus(): whether to raise a
 * desktop notification, and whether the chat on screen counts as read. WebKit
 * answers it from the WebView's own state, which stays "focused" while the
 * window sits hidden in the tray -- so the page is told what GTK knows instead.
 *
 * Note what this is not: an earlier version pinned the page's answer to false to
 * force notifications through, which also told WhatsApp the user was never
 * looking and silently killed read receipts. Reporting the real state gives both
 * -- notifications while the window is away, receipts while it is in front. */
static void
push_focus_state(WaApp *self)
{
    if (!self->view || !self->window)
        return;

    const gboolean focused = gtk_widget_get_visible(GTK_WIDGET(self->window)) &&
                             gtk_window_is_active(self->window);
    char *js = g_strdup_printf("window.__whatsappSetFocus && window.__whatsappSetFocus(%s);",
                               focused ? "true" : "false");
    webkit_web_view_evaluate_javascript(self->view, js, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(js);
}

static void
on_window_state_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    WaApp *self = user_data;

    push_focus_state(self);
}

/* inject.js starts every page load with no idea where the focus is, so the state
 * is pushed again as soon as there is a page to receive it. */
static void
on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer user_data)
{
    WaApp *self = user_data;

    if (event == WEBKIT_LOAD_FINISHED) {
        self->loaded_at = g_get_monotonic_time();
        push_focus_state(self);
    }
}

/* ------------------------------------------------------------------- debug */

/* A maintenance channel: JavaScript dropped into the file named by
 * WHATSAPP_DEBUG_EVAL is run in the live page and its result logged. WebKitGTK
 * 2.52's remote inspector does not answer on its HTTP port, and questions like
 * "what does document.hasFocus() return while the window sits in the tray" can
 * only be answered in a signed-in session, not in a test harness.
 *
 * Unset by default, and deliberately so: it is a way into a live WhatsApp
 * session, not a feature. */
static void
on_eval_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GError *error = NULL;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(source), result, &error);

    if (!value) {
        g_message("[eval] failed: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        return;
    }

    char *text = jsc_value_to_string(value);
    g_message("[eval] %s", text);
    g_free(text);
    g_object_unref(value);
}

/* A picture of what is actually on screen. GNOME's Wayland session refuses
 * screenshots to a process like this one, and questions about text shaping or
 * emoji cannot be settled from the DOM -- getComputedStyle says what was asked
 * for, not what was drawn. */
static void
on_snapshot_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GError *error = NULL;
    GdkTexture *shot = webkit_web_view_get_snapshot_finish(
        WEBKIT_WEB_VIEW(source), result, &error);

    if (!shot) {
        g_message("[snapshot] failed: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        return;
    }

    const char *path = "/tmp/whatsapp-snapshot.png";
    if (gdk_texture_save_to_png(shot, path))
        g_message("[snapshot] wrote %dx%d to %s",
                  gdk_texture_get_width(shot), gdk_texture_get_height(shot), path);
    else
        g_message("[snapshot] could not write %s", path);

    g_object_unref(shot);
}

static void
on_debug_file_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                      GFileMonitorEvent event, gpointer user_data)
{
    WaApp *self = user_data;

    if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
        event != G_FILE_MONITOR_EVENT_CREATED)
        return;

    char *script = NULL;
    if (!g_file_load_contents(file, NULL, &script, NULL, NULL, NULL))
        return;

    if (g_str_has_prefix(g_strstrip(script), "#snapshot")) {
        webkit_web_view_get_snapshot(self->view, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                     WEBKIT_SNAPSHOT_OPTIONS_NONE, NULL,
                                     on_snapshot_ready, self);
        g_free(script);
        return;
    }

    if (*g_strstrip(script))
        webkit_web_view_evaluate_javascript(self->view, script, -1, NULL, NULL,
                                            NULL, on_eval_done, self);
    g_free(script);
}

static void
watch_debug_file(WaApp *self)
{
    const char *path = g_getenv("WHATSAPP_DEBUG_EVAL");
    if (!path)
        return;

    GFile *file = g_file_new_for_path(path);
    self->debug_monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, NULL);
    g_object_unref(file);

    if (!self->debug_monitor) {
        g_warning("debug: could not watch %s", path);
        return;
    }
    g_signal_connect(self->debug_monitor, "changed",
                     G_CALLBACK(on_debug_file_changed), self);
    g_message("debug: evaluating %s in the page whenever it changes", path);
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

/* WhatsApp puts a "Message notifications are off. Turn on" banner across the top
 * of the chat list whenever Notification.permission is not already "granted".
 * WebKitGTK does not persist that grant across restarts, so pressing "Turn on"
 * worked for exactly one session and the banner was back on the next launch --
 * every launch, no matter what the in-app settings said. Seeding the origin as
 * allowed before the first load is what makes the permission true at first paint
 * rather than after a prompt. */
static void
allow_notifications(WebKitWebView *view)
{
    WebKitSecurityOrigin *origin = webkit_security_origin_new_for_uri(WA_URL);
    GList *allowed = g_list_prepend(NULL, origin);

    webkit_web_context_initialize_notification_permissions(
        webkit_web_view_get_context(view), allowed, NULL);

    g_list_free(allowed);
    webkit_security_origin_unref(origin);
}

/* navigator.permissions.query() takes this path rather than the request one, and
 * an unanswered query leaves WhatsApp believing notifications are unavailable. */
static gboolean
on_permission_state_query(WebKitWebView *view, WebKitPermissionStateQuery *query,
                          gpointer user_data)
{
    const char *name = webkit_permission_state_query_get_name(query);

    webkit_permission_state_query_finish(
        query, g_strcmp0(name, "notifications") == 0 ? WEBKIT_PERMISSION_STATE_GRANTED
                                                     : WEBKIT_PERMISSION_STATE_PROMPT);
    return TRUE;
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

/* ------------------------------------------------------------------- links */

/* Every link in a chat is target="_blank", and WebKit answers that by asking the
 * application for a second web view. Without a handler the request is simply
 * dropped, which is why clicking a link did nothing at all -- no new window, no
 * navigation, no error. There is nothing to open a second view for here: a link
 * in a message belongs in the browser, the way it does in every other desktop
 * chat client. */
static gboolean
uri_opens_externally(const char *uri)
{
    if (!uri || !*uri)
        return FALSE;
    /* blob: and data: are how WhatsApp hands over a download, and about:blank is
     * the placeholder a window.open() starts life as; none of them mean anything
     * to a browser. */
    return g_str_has_prefix(uri, "http://")  || g_str_has_prefix(uri, "https://") ||
           g_str_has_prefix(uri, "mailto:")  || g_str_has_prefix(uri, "tel:")     ||
           g_str_has_prefix(uri, "callto:");
}

/* The client itself lives on web.whatsapp.com and loads from whatsapp.net, so
 * those stay inside; anything else a click leads to is the web at large. */
static gboolean
uri_is_the_client(const char *uri)
{
    GUri *parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
    if (!parsed)
        return FALSE;

    const char *host = g_uri_get_host(parsed);
    const gboolean ours = host && (g_str_has_suffix(host, "whatsapp.com") ||
                                   g_str_has_suffix(host, "whatsapp.net"));
    g_uri_unref(parsed);
    return ours;
}

static void
open_in_browser(const char *uri)
{
    if (!uri_opens_externally(uri)) {
        g_message("not opening %s outside the app", uri ? uri : "(null)");
        return;
    }

    GError *error = NULL;
    if (g_app_info_launch_default_for_uri(uri, NULL, &error))
        g_message("opened %s in the browser", uri);
    else {
        g_message("could not open %s: %s", uri, error ? error->message : "unknown");
        g_clear_error(&error);
    }
}

/* window.open() from the page, and any <a target="_blank"> WebKit turns into
 * one. Returning NULL declines the new view; the URI has already been handed to
 * the browser by then. */
static GtkWidget *
on_create_web_view(WebKitWebView *view, WebKitNavigationAction *action, gpointer user_data)
{
    WebKitURIRequest *request = webkit_navigation_action_get_request(action);
    open_in_browser(request ? webkit_uri_request_get_uri(request) : NULL);
    return NULL;
}

/* The same click can also arrive as a policy decision rather than as a request
 * for a view, and a plain link with no target would otherwise navigate the
 * client itself away from WhatsApp -- leaving the user with a web page and no
 * way back. Both are sent to the browser instead. */
static gboolean
on_decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision,
                 WebKitPolicyDecisionType type, gpointer user_data)
{
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
        type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
        return FALSE;

    WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(
        WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    WebKitURIRequest *request = webkit_navigation_action_get_request(action);
    const char *uri = request ? webkit_uri_request_get_uri(request) : NULL;

    if (!uri_opens_externally(uri))
        return FALSE;

    /* A navigation the page made on its own -- a redirect through a login step,
     * say -- is the client going about its business, and only a click is a
     * request to leave. */
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
        (uri_is_the_client(uri) || !webkit_navigation_action_is_user_gesture(action)))
        return FALSE;

    open_in_browser(uri);
    webkit_policy_decision_ignore(decision);
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
    /* Strict first: each setter asserts that conservative < strict against the
     * value currently held, so raising conservative above the default strict
     * (0.5) is rejected outright and silently leaves the old value in place. */
    webkit_memory_pressure_settings_set_strict_threshold(pressure, WA_STRICT_FRAC);
    webkit_memory_pressure_settings_set_conservative_threshold(pressure, WA_CONSERVATIVE_FRAC);
    /* Zero disables the kill threshold: shedding caches is fine, losing the
     * window because a chat had too many photos in it is not. */
    webkit_memory_pressure_settings_set_kill_threshold(pressure, 0.0);
    webkit_memory_pressure_settings_set_poll_interval(pressure, WA_POLL_INTERVAL_SECS);

    webkit_network_session_set_memory_pressure_settings(pressure);
    webkit_memory_pressure_settings_free(pressure);
}

/* WhatsApp Web raises its own desktop notification whenever it believes the
 * window is unfocused, and stays silent when it does not -- so this fills in
 * exactly that gap and never doubles up. Forcing the page to always believe
 * itself unfocused was tried first and is a trap: it does produce notifications,
 * and it also convinces WhatsApp the user is not looking, so opening a chat
 * never marks it read. */
/* GNotification carries no sound, and GNOME only rings for notifications that
 * come with a sound hint, so the tone is played here. This is the ding that went
 * missing when the hasFocus override was dropped: WhatsApp Web used to believe
 * it was permanently unfocused and so played its own tone for every message.
 * canberra-gtk-play follows the desktop's sound theme; paplay is the fallback
 * for a system without libcanberra installed. */
static void
play_message_sound(void)
{
    GSettings *sound = NULL;
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    GSettingsSchema *schema = source ? g_settings_schema_source_lookup(
        source, "org.gnome.desktop.sound", TRUE) : NULL;

    if (schema) {
        sound = g_settings_new("org.gnome.desktop.sound");
        g_settings_schema_unref(schema);
    }
    /* Someone who has switched desktop sounds off means it. */
    if (sound && !g_settings_get_boolean(sound, "event-sounds")) {
        g_object_unref(sound);
        return;
    }
    g_clear_object(&sound);

    char *canberra = g_find_program_in_path("canberra-gtk-play");
    char *argv_canberra[] = { canberra, "-i", "message-new-instant",
                              "-d", "new WhatsApp message", NULL };
    char *argv_paplay[]   = { "paplay",
                              "/usr/share/sounds/freedesktop/stereo/message-new-instant.oga",
                              NULL };
    char **argv = canberra ? argv_canberra : argv_paplay;

    GError *error = NULL;
    if (!g_spawn_async(NULL, argv, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                       G_SPAWN_STDERR_TO_DEV_NULL,
                       NULL, NULL, NULL, &error)) {
        g_message("no notification sound: %s", error ? error->message : "unknown");
        g_clear_error(&error);
    }
    g_free(canberra);
}

typedef struct {
    WaApp   *app;
    WaSource source;
} UnreadNotice;

/* The page hands over the contact's picture as raw bytes; a notification icon
 * has to be a file, so it is written to the runtime directory -- named after the
 * picture itself.
 *
 * The name used to rotate through eight slots, and that was a bug the user
 * caught with a screenshot: a notification carries the PATH of its picture, and
 * the shell reads that path lazily -- when it draws the banner, and again every
 * time the notification centre redraws the entry. Nineteen messages in a burst
 * went round the eight names twice, so files were rewritten under notifications
 * that were still on screen and one sender's message appeared under another
 * sender's face. A name taken from the bytes cannot collide: the same face is
 * written once and every notification pointing at it stays correct for as long
 * as it lives. */
static char *
avatar_path(const char *base64)
{
    gsize length = 0;
    guchar *bytes = g_base64_decode(base64, &length);
    if (!bytes || length == 0) {
        g_free(bytes);
        return NULL;
    }

    char *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes, length);
    char *name = g_strdup_printf("%s%.16s", WA_AVATAR_PREFIX, digest);
    char *path = g_build_filename(g_get_user_runtime_dir(), name, NULL);
    g_free(digest);
    g_free(name);

    gboolean written = g_file_test(path, G_FILE_TEST_EXISTS) ||
                       g_file_set_contents(path, (const char *)bytes, length, NULL);
    g_free(bytes);

    if (!written) {
        g_free(path);
        return NULL;
    }
    return path;
}

/* The pictures of a whole session add up, and nothing else ever deletes them --
 * a notification may still be pointing at one, so they cannot be cleaned up
 * while the client runs. Startup is the safe moment: whatever is on screen then
 * belongs to a client that is no longer running. */
static void
avatars_sweep(void)
{
    GDir *dir = g_dir_open(g_get_user_runtime_dir(), 0, NULL);
    const char *name;
    int removed = 0;

    if (!dir)
        return;

    while ((name = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(name, WA_AVATAR_PREFIX))
            continue;
        char *path = g_build_filename(g_get_user_runtime_dir(), name, NULL);
        if (g_remove(path) == 0)
            removed++;
        g_free(path);
    }
    g_dir_close(dir);

    if (removed)
        g_message("cleared %d notification picture%s from the last session",
                  removed, removed == 1 ? "" : "s");
}

static void show_window(WaApp *self);

/* ----------------------------------------------------------------- banners */

/* One Notify call, kept alive across the round trip so a refused id can be
 * raised again as a fresh notification rather than dropped. */
typedef struct {
    WaApp              *app;
    int                 slot;
    char               *summary;
    char               *body;
    char               *image;
    WebKitNotification *page;
    guint32             replaced;
    gboolean            quiet;   /* for the notification centre only: no banner */
    gboolean            retried;
} WaNotifyCall;

static void
notify_call_free(WaNotifyCall *call)
{
    g_clear_object(&call->page);
    g_free(call->summary);
    g_free(call->body);
    g_free(call->image);
    g_free(call);
}

static void notify_send(WaNotifyCall *call, guint32 replaces);

/* Files what a banner is currently saying into the notification centre, and
 * optionally takes the banner down with it.
 *
 * A LOW-urgency notification is filed by the shell without a banner and without
 * a sound, which is exactly what a copy of something already announced should
 * do. Every message passes through here exactly once: when the next message
 * from the same conversation replaces its banner, or when its banner runs out
 * of time. A banner the user dismissed or clicked is not archived -- they have
 * seen it, and putting it back where they just cleared it is rude. */
static void
banner_archive(WaApp *self, WaBanner *slot, gboolean close_it)
{
    if (slot->expiry) {
        g_source_remove(slot->expiry);
        slot->expiry = 0;
    }
    if (slot->id == 0 || !slot->chat || !slot->body)
        return;

    WaNotifyCall *copy = g_new0(WaNotifyCall, 1);
    copy->app     = self;
    copy->slot    = (int)(slot - self->banners);
    copy->summary = g_strdup(slot->chat);
    copy->body    = g_strdup(slot->body);
    copy->image   = g_strdup(slot->image);
    copy->page    = slot->page ? g_object_ref(slot->page) : NULL;
    copy->quiet   = TRUE;
    notify_send(copy, 0);

    if (!close_it)
        return;

    GDBusConnection *bus = g_application_get_dbus_connection(G_APPLICATION(self->app));
    if (bus)
        g_dbus_connection_call(
            bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications", "CloseNotification",
            g_variant_new("(u)", slot->id), NULL,
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    slot->id = 0;
}

/* The banner this conversation is already holding, if it still has one. */
static WaBanner *
banner_for_chat(WaApp *self, const char *chat)
{
    const gint64 now = g_get_monotonic_time();

    for (int i = 0; i < WA_BANNERS; i++)
        if (self->banners[i].chat && g_strcmp0(self->banners[i].chat, chat) == 0 &&
            now - self->banners[i].at < WA_BANNER_REUSE_US)
            return &self->banners[i];
    return NULL;
}

/* The slot the next banner for this conversation goes in: its own if it has one,
 * otherwise the oldest. What the banner says is kept with it, because the
 * message is filed in the notification centre when something replaces it. */
static WaBanner *
banner_take(WaApp *self, const char *chat, const char *body, const char *image,
            WebKitNotification *page)
{
    WaBanner *slot = banner_for_chat(self, chat);

    if (slot) {
        /* Replacing this conversation's banner. What it says now goes into the
         * notification centre first: three messages from one person leave three
         * entries behind, and only the newest of them is ever on screen. */
        banner_archive(self, slot, FALSE);
    } else {
        slot = &self->banners[self->banner_next];
        self->banner_next = (self->banner_next + 1) % WA_BANNERS;
        /* The conversation being turned out of this slot may still have a banner
         * up; it is filed and closed rather than abandoned on screen. */
        banner_archive(self, slot, TRUE);
        g_free(slot->chat);
        slot->chat = g_strdup(chat);
        slot->id = 0;
    }

    g_free(slot->body);
    g_free(slot->image);
    slot->body  = g_strdup(body);
    slot->image = g_strdup(image);

    g_clear_object(&slot->page);
    slot->page = page ? g_object_ref(page) : NULL;
    slot->at = g_get_monotonic_time();
    return slot;
}

/* Which notification a click belongs to. Banners come and go, and an entry left
 * in the notification centre can be clicked hours later, so the ids are kept in
 * a ring of their own rather than only on the conversation's slot. */
static void
clickable_add(WaApp *self, guint32 id, WebKitNotification *page)
{
    WaClickable *entry = &self->clickable[self->clickable_next];

    self->clickable_next = (self->clickable_next + 1) % WA_CLICKABLE;
    g_clear_object(&entry->page);
    entry->id   = id;
    entry->page = page ? g_object_ref(page) : NULL;
}

/* Clicking a notification raises the window -- and, when it came from one the
 * page raised, hands the click back to the page, which opens the conversation
 * it belongs to. Without that the window comes up on whatever was last read
 * rather than on the message just announced. */
static void
on_notification_action(GDBusConnection *bus, const char *sender, const char *path,
                       const char *iface, const char *signal, GVariant *params,
                       gpointer user_data)
{
    WaApp *self = user_data;
    guint32 id = 0;
    const char *action = NULL;

    g_variant_get(params, "(u&s)", &id, &action);
    if (id == 0)
        return;

    for (int i = 0; i < WA_CLICKABLE; i++)
        if (self->clickable[i].id == id) {
            if (self->clickable[i].page)
                webkit_notification_clicked(self->clickable[i].page);
            show_window(self);
            return;
        }
}

/* A notification the desktop has closed -- dismissed, clicked, or cleared out of
 * the notification centre -- is finished with. Its id must not be offered back
 * as something to replace, and it must not be filed away again behind the user. */
static void
on_notification_closed(GDBusConnection *bus, const char *sender, const char *path,
                       const char *iface, const char *signal, GVariant *params,
                       gpointer user_data)
{
    WaApp *self = user_data;
    guint32 id = 0, reason = 0;

    g_variant_get(params, "(uu)", &id, &reason);
    if (id == 0)
        return;

    for (int i = 0; i < WA_BANNERS; i++)
        if (self->banners[i].id == id) {
            self->banners[i].id = 0;
            if (self->banners[i].expiry) {
                g_source_remove(self->banners[i].expiry);
                self->banners[i].expiry = 0;
            }
        }
}

/* GNOME rings for the sound-name hint and says so; a desktop whose daemon does
 * not advertise the capability is rung for by hand instead. */
static void
on_capabilities(GObject *source, GAsyncResult *result, gpointer user_data)
{
    WaApp *self = user_data;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
    if (!reply)
        return;

    GVariantIter *iter = NULL;
    const char *capability = NULL;
    gboolean sound = FALSE;

    g_variant_get(reply, "(as)", &iter);
    while (iter && g_variant_iter_next(iter, "&s", &capability))
        if (g_strcmp0(capability, "sound") == 0)
            sound = TRUE;
    if (iter)
        g_variant_iter_free(iter);
    g_variant_unref(reply);

    self->notify_has_sound = sound;
    g_message("notification sound: %s", sound ? "the desktop rings for us" : "played here");
}

static void
notify_subscribe(WaApp *self, GDBusConnection *bus)
{
    if (self->notify_subscribed)
        return;
    self->notify_subscribed = TRUE;
    /* Assumed until the daemon answers, so a slow reply cannot ring twice. */
    self->notify_has_sound = TRUE;

    g_dbus_connection_signal_subscribe(
        bus, "org.freedesktop.Notifications", "org.freedesktop.Notifications",
        "ActionInvoked", "/org/freedesktop/Notifications", NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_notification_action, self, NULL);
    g_dbus_connection_signal_subscribe(
        bus, "org.freedesktop.Notifications", "org.freedesktop.Notifications",
        "NotificationClosed", "/org/freedesktop/Notifications", NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_notification_closed, self, NULL);
    g_dbus_connection_call(
        bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "GetCapabilities", NULL,
        G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        on_capabilities, self);
}

/* Takes a banner down when the desktop will not, and leaves the message behind
 * in the notification centre.
 *
 * GNOME reads the expire_timeout of a notification and throws it away: a banner
 * comes down when the user has been active AND the pointer is not resting on it,
 * and until then it stays. That is a fine policy for one notification and a trap
 * for a messenger, because the shell shows one banner at a time, queues three
 * behind it and drops everything after that on the floor -- so one banner parked
 * under an idle mouse pointer silently swallows every message that follows,
 * sound and all. Measured on this desktop: with one of ours stuck, six
 * notifications in a row produced no banner and no sound, including one sent at
 * CRITICAL urgency; the moment it went away, the next one rang. */
static gboolean
banner_expire(gpointer user_data)
{
    WaBannerExpiry *expiry = user_data;
    WaApp *self = expiry->app;
    WaBanner *slot = &self->banners[expiry->slot];

    slot->expiry = 0;

    /* A different id in the slot means this banner is already gone -- clicked,
     * dismissed, or replaced -- and the id now belongs to somebody else's
     * notification, which is not ours to close. */
    if (slot->id != expiry->id || slot->id == 0)
        return G_SOURCE_REMOVE;

    g_message("banner taken down after %ds; %s keeps its place in the notification centre",
              WA_BANNER_LIFETIME_S, slot->chat ? slot->chat : WA_TITLE);
    banner_archive(self, slot, TRUE);
    return G_SOURCE_REMOVE;
}

static void
on_notify_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
    WaNotifyCall *call = user_data;
    WaApp *self = call->app;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    if (reply) {
        guint32 id = 0;
        g_variant_get(reply, "(u)", &id);
        g_variant_unref(reply);
        clickable_add(self, id, call->page);

        /* The quiet copy is an entry in the notification centre and nothing
         * more: there is no banner on it to take down, and the conversation's
         * next message must start a banner of its own rather than replace it --
         * which is what leaves one entry behind per message. */
        if (!call->quiet) {
            WaBanner *slot = &self->banners[call->slot];
            slot->id = id;
            if (slot->expiry)
                g_source_remove(slot->expiry);

            WaBannerExpiry *expiry = g_new0(WaBannerExpiry, 1);
            expiry->app  = self;
            expiry->slot = call->slot;
            expiry->id   = id;
            slot->expiry = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, WA_BANNER_LIFETIME_S,
                                                      banner_expire, expiry, g_free);
        }
        notify_call_free(call);
        return;
    }

    /* A daemon that has handed the id on, or forgotten it, answers "Invalid
     * notification ID". Leaving it at that would take every later message from
     * this conversation with it, since the same id would be offered again. */
    g_message("notification refused: %s%s", error ? error->message : "unknown",
              call->replaced && !call->retried ? " -- raising it fresh" : "");
    g_clear_error(&error);
    if (!call->quiet)
        self->banners[call->slot].id = 0;

    if (call->replaced && !call->retried) {
        call->retried = TRUE;
        notify_send(call, 0);
        return;
    }
    notify_call_free(call);
}

/* The desktop's own notification interface rather than GNotification, for the
 * hints GNotification cannot express: sound-name, which is what rings (a
 * GNotification is silent), the urgency that tells a quiet copy from a banner,
 * and the picture of whoever sent the message.
 *
 * Notifications are deliberately NOT transient: they stay in the notification
 * centre after the banner has gone, which is where the user goes looking for a
 * message they missed. */
static void
notify_send(WaNotifyCall *call, guint32 replaces)
{
    GDBusConnection *bus = g_application_get_dbus_connection(G_APPLICATION(call->app->app));
    if (!bus) {
        notify_call_free(call);
        return;
    }

    GVariantBuilder actions;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&actions, "s", "default");
    g_variant_builder_add(&actions, "s", "Open");

    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(WA_APP_ID));
    g_variant_builder_add(&hints, "{sv}", "category", g_variant_new_string("im.received"));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(call->quiet ? 0 : 1));
    if (!call->quiet)
        g_variant_builder_add(&hints, "{sv}", "sound-name",
                              g_variant_new_string("message-new-instant"));
    if (call->image)
        g_variant_builder_add(&hints, "{sv}", "image-path", g_variant_new_string(call->image));

    call->replaced = replaces;
    g_dbus_connection_call(
        bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susssasa{sv}i)", WA_TITLE, replaces,
                      call->image ? call->image : WA_ICON_NAME, call->summary, call->body,
                      &actions, &hints, -1),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        on_notify_sent, call);
}

/* Where GNotification is all there is -- no session bus of our own -- which in
 * practice means a desktop this client was never built for. Silent, and without
 * a picture, but it is a message the user would otherwise never see. */
static void
notify_through_gio(WaApp *self, const char *summary, const char *body)
{
    GNotification *note = g_notification_new(summary);
    GIcon *icon = g_themed_icon_new(WA_ICON_NAME);

    g_notification_set_body(note, body);
    g_notification_set_icon(note, icon);
    g_notification_set_default_action(note, "app.present");
    g_application_send_notification(G_APPLICATION(self->app), "unread", note);
    play_message_sound();

    g_object_unref(icon);
    g_object_unref(note);
}

/* The same message can reach this point twice: the chat list watcher reports
 * every arrival, and the document title reports the first one in a chat whose
 * row the watcher could not see. Two reports from two different places are one
 * message; two from the same place are two messages, and collapsing those is
 * what left the second and third message of a burst unannounced. */
static gboolean
notification_is_repeat(WaApp *self, const char *summary, const char *body, WaSource source)
{
    const gint64 now = g_get_monotonic_time();
    const gboolean same = g_strcmp0(self->last_summary, summary) == 0 &&
                          g_strcmp0(self->last_body, body) == 0;
    const gboolean repeat = same && source != self->last_source &&
                            now - self->last_notify_at < WA_NOTIFY_DEDUPE_US;

    g_free(self->last_summary);
    g_free(self->last_body);
    self->last_summary   = g_strdup(summary);
    self->last_body      = g_strdup(body);
    self->last_source    = source;
    self->last_notify_at = now;
    return repeat;
}

/* Every banner in the client comes through here, wherever it was decided. */
static void
wa_notify(WaApp *self, const char *chat, const char *body, const char *image,
          WebKitNotification *page, WaSource source)
{
    const char *summary = (chat && *chat) ? chat : WA_TITLE;

    /* Never a banner with nothing to say. "You have a new message" under no
     * sender and no text is what every phantom looked like, and a banner the
     * client cannot fill in is one the user cannot act on either. */
    if (!body || !*body) {
        g_message("notification skipped: nothing to say");
        return;
    }
    if (notification_is_repeat(self, summary, body, source)) {
        g_message("notification skipped: just announced from elsewhere");
        return;
    }

    GDBusConnection *bus = g_application_get_dbus_connection(G_APPLICATION(self->app));
    if (!bus) {
        notify_through_gio(self, summary, body);
        return;
    }
    notify_subscribe(self, bus);

    WaBanner *slot = banner_take(self, summary, body, image, page);
    WaNotifyCall *call = g_new0(WaNotifyCall, 1);
    call->app     = self;
    call->slot    = (int)(slot - self->banners);
    call->summary = g_strdup(summary);
    call->body    = g_strdup(body);
    call->image   = g_strdup(image);
    call->page    = page ? g_object_ref(page) : NULL;

    g_message("notification: %s -- %s", summary, body);
    notify_send(call, slot->id);

    if (!self->notify_has_sound)
        play_message_sound();
}

/* --------------------------------------------- notifications the page raises */

typedef struct {
    WaApp              *app;
    WebKitNotification *note;
} PageNotice;

/* WhatsApp Web raises its own notification whenever it believes the window is
 * not in front, and it is the better judge by far: it knows the sender, the
 * text, whether the chat is muted, and that what just landed is a message
 * rather than a typing indicator or something the user sent from their phone.
 * What it cannot do is dress it -- WebKit's default handler shows a silent
 * banner that stays in the tray -- so the decision is left to the page and the
 * banner is raised here, with a sound, a face and a click that opens the chat. */
static void
on_page_avatar(GObject *source, GAsyncResult *result, gpointer user_data)
{
    PageNotice *notice = user_data;
    JSCValue *value = webkit_web_view_call_async_javascript_function_finish(
        WEBKIT_WEB_VIEW(source), result, NULL);
    char *image = NULL;

    if (value) {
        char *b64 = jsc_value_to_string(value);
        if (b64 && *b64 && !g_str_equal(b64, "undefined") && !g_str_equal(b64, "null"))
            image = avatar_path(b64);
        g_free(b64);
        g_object_unref(value);
    }

    wa_notify(notice->app,
              webkit_notification_get_title(notice->note),
              webkit_notification_get_body(notice->note),
              image, notice->note, WA_SOURCE_PAGE);

    g_free(image);
    g_object_unref(notice->note);
    g_free(notice);
}

static gboolean
on_show_notification(WebKitWebView *view, WebKitNotification *note, gpointer user_data)
{
    WaApp *self = user_data;
    const char *title = webkit_notification_get_title(note);

    PageNotice *notice = g_new0(PageNotice, 1);
    notice->app  = self;
    notice->note = g_object_ref(note);

    /* A WebKitNotification carries text and nothing else, so the picture is
     * fetched by name from the chat list. The lookup answers an empty string
     * when it cannot find the row or the fetch takes too long, and the banner
     * goes out under the app icon instead of waiting on it. */
    GVariantBuilder args;
    g_variant_builder_init(&args, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&args, "{sv}", "name", g_variant_new_string(title ? title : ""));
    GVariant *arguments = g_variant_ref_sink(g_variant_builder_end(&args));

    webkit_web_view_call_async_javascript_function(
        view, "return window.__whatsappAvatarFor ? window.__whatsappAvatarFor(name) : '';",
        -1, arguments, NULL, NULL, NULL, on_page_avatar, notice);
    g_variant_unref(arguments);

    return TRUE;   /* raised here, so WebKit must not raise it as well */
}

/* --------------------------------------- notifications the chat list decides */

/* Whether a banner is this client's to raise at all. While the window is away
 * the page raises its own and on_show_notification dresses it; the watcher has
 * to stay out of that, or one message arrives twice.
 *
 * The one grace period left is the unread backlog syncing in after a cold start.
 * There was a second, over the moment the window came back from the tray, and it
 * had to go: it swallowed a whole burst of group messages during a live test,
 * because switching windows restamped it every few seconds. What it guarded
 * against -- the list rewriting itself on a re-render -- is the page-side
 * freshness test's job, and that one cannot mistake a rewrite for a message. */
static gboolean
banners_are_ours(WaApp *self)
{
    const gint64 now = g_get_monotonic_time();

    if (!self->window || !gtk_widget_get_visible(GTK_WIDGET(self->window)) ||
        !gtk_window_is_active(self->window))
        return FALSE;
    if (now - self->loaded_at < WA_STARTUP_GRACE_US) {
        g_message("notification skipped: the client is still syncing");
        return FALSE;
    }
    return TRUE;
}

/* "open" is the page saying the message landed in the conversation already on
 * screen. Nothing is raised for that: the user is reading it as it arrives, and
 * WhatsApp plays its own arrival tone, which is the whole of what a notification
 * would add. Banners are for the chats the user is not looking at. */
static void
on_unread_described(GObject *source, GAsyncResult *result, gpointer user_data)
{
    UnreadNotice *notice = user_data;
    GError *error = NULL;
    JSCValue *value = webkit_web_view_call_async_javascript_function_finish(
        WEBKIT_WEB_VIEW(source), result, &error);

    char *chat = NULL, *sender = NULL, *message = NULL, *avatar = NULL, **parts = NULL;
    gboolean on_screen = FALSE;

    if (value) {
        char *payload = jsc_value_to_string(value);
        if (payload && g_str_equal(payload, "open")) {
            on_screen = TRUE;
        } else if (payload && *payload && !g_str_equal(payload, "undefined")) {
            parts = g_strsplit(payload, "\x1f", 4);
            chat    = (parts[0] && *parts[0]) ? parts[0] : NULL;
            sender  = (chat && parts[1] && *parts[1]) ? parts[1] : NULL;
            message = (chat && parts[2] && *parts[2]) ? parts[2] : NULL;
            avatar  = (chat && parts[3] && *parts[3]) ? parts[3] : NULL;
        }
        g_free(payload);
        g_object_unref(value);
    } else {
        g_message("could not ask the page what arrived: %s",
                  error ? error->message : "unknown");
        g_clear_error(&error);
    }

    if (on_screen) {
        g_message("notification skipped: the message is in the chat on screen");
    } else if (!chat || !message) {
        /* The list moved but the page cannot say what moved it. Whatever it was
         * -- a row mid-render, a reaction, a chat being read somewhere else --
         * it is not something to put a banner over. */
        g_message("notification skipped: nothing the page could name");
    } else {
        char *image = avatar ? avatar_path(avatar) : NULL;
        char *line  = sender ? g_strdup_printf("%s: %s", sender, message)
                             : g_strdup(message);
        wa_notify(notice->app, chat, line, image, NULL, notice->source);
        g_free(line);
        g_free(image);
    }

    g_strfreev(parts);
    g_free(notice);
}

/* The page is asked for the description at notification time rather than pushing
 * it ahead of time. An earlier version had inject.js post it on every title
 * change and read whatever had last arrived, which raced the title itself: the
 * count reached the app first and every notification read "You have a new
 * message" with no sender and no text. The short delay lets WhatsApp finish
 * moving the chat to the top of the list before the row is read. */
static gboolean
describe_then_notify(gpointer user_data)
{
    UnreadNotice *notice = user_data;

    webkit_web_view_call_async_javascript_function(
        notice->app->view,
        "return window.__whatsappDescribeUnread ? window.__whatsappDescribeUnread() : '';",
        -1, NULL, NULL, NULL, NULL, on_unread_described, notice);
    return G_SOURCE_REMOVE;
}

/* The chat list watcher nudges us here for every message it sees land, which is
 * what makes a banner per message possible at all. The document title cannot do
 * that job: its number counts unread CHATS, so the second and third message from
 * one person leave "(1) WhatsApp" exactly as it was and nothing fires. */
static void
on_page_event(WebKitUserContentManager *manager, JSCValue *value, gpointer user_data)
{
    WaApp *self = user_data;
    char *text = jsc_value_to_string(value);

    if (g_strcmp0(text, "arrival") == 0 && banners_are_ours(self)) {
        self->last_arrival_at = g_get_monotonic_time();

        UnreadNotice *notice = g_new0(UnreadNotice, 1);
        notice->app    = self;
        notice->source = WA_SOURCE_WATCHER;
        g_timeout_add(250, describe_then_notify, notice);
    }
    g_free(text);
}

/* WhatsApp Web puts "(3) WhatsApp" in the document title while chats are unread
 * and drops the prefix once they are read. That is the only unread signal the
 * page hands us without scraping its DOM, and it is what marks the tray icon. */
static void
on_title_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    WaApp *self = user_data;
    const char *title = webkit_web_view_get_title(self->view);

    /* The parenthesised number counts unread CHATS, not messages: two
     * conversations holding five messages between them read "(2) WhatsApp". It
     * is read for one thing only -- has anything new arrived -- because that is
     * all it is reliable for. No number is shown anywhere. */
    int chats = 0;
    if (title && title[0] == '(')
        chats = atoi(title + 1) > 0 ? atoi(title + 1) : 1;

    /* A backstop for the one case the chat list watcher cannot see: a chat far
     * enough down the list that its row was never rendered has no previous
     * preview to have changed, so nothing is reported when a message moves it to
     * the top. The count rises all the same. Anything the watcher already
     * announced within the last couple of seconds is left alone. */
    const gint64 now = g_get_monotonic_time();
    if (chats > self->unread_chats &&
        now - self->last_arrival_at > WA_TITLE_FALLBACK_US &&
        banners_are_ours(self)) {
        UnreadNotice *notice = g_new0(UnreadNotice, 1);
        notice->app    = self;
        notice->source = WA_SOURCE_TITLE;
        g_timeout_add(250, describe_then_notify, notice);
    }
    self->unread_chats = chats;

    wa_tray_set_attention(self->tray, chats > 0);

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

    char *data_dir  = g_build_filename(g_get_user_data_dir(),  "whatsapp", NULL);
    char *cache_dir = g_build_filename(g_get_user_cache_dir(), "whatsapp", NULL);
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
    webkit_user_content_manager_register_script_message_handler(content, "whatsapp", NULL);
    g_signal_connect(content, "script-message-received::whatsapp",
                     G_CALLBACK(on_script_message), self);
    webkit_user_content_manager_register_script_message_handler(content, "whatsappPaste", NULL);
    g_signal_connect(content, "script-message-received::whatsappPaste",
                     G_CALLBACK(on_paste_request), self);
    webkit_user_content_manager_register_script_message_handler(content, "whatsappEvent", NULL);
    g_signal_connect(content, "script-message-received::whatsappEvent",
                     G_CALLBACK(on_page_event), self);

    char *font_spec = resolve_font(self);
    apply_font(settings, content, font_spec, resolve_font_size(self));
    g_free(font_spec);

    WebKitUserScript *script = webkit_user_script_new(
        WA_INJECT_JS,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        NULL, NULL);
    webkit_user_content_manager_add_script(content, script);
    webkit_user_script_unref(script);

    /* Emoji are sprite sheets, and WhatsApp picks which one to fetch from the
     * display's resolution: at 1x it serves 40px tiles. Zooming the view does not
     * change that -- WebKit re-lays-out text at the new size, so text stays sharp,
     * but a 40px bitmap stretched across 60 device pixels is exactly the softness
     * in the emoji panel.
     *
     * Two things were measured before settling on this. Overriding
     * devicePixelRatio alone changes nothing: WhatsApp asks
     * matchMedia('(min-resolution: ...)'), and the 152 generated sprite rules
     * follow that answer. And GDK_SCALE=2 does nothing under Wayland, where the
     * compositor owns the scale factor and this display runs at 1.
     *
     * So the page is told the resolution it is really being drawn at. Resolution
     * queries are the only ones answered here; everything else, prefers-color-
     * scheme included, goes through to WebKit untouched. */
    if (zoom >= 1.25) {
        char *hidpi = g_strdup_printf(
            "(() => {"
            "  const ratio = %d;"
            "  Object.defineProperty(window, 'devicePixelRatio',"
            "    { value: ratio, configurable: true });"
            "  const real = window.matchMedia.bind(window);"
            "  window.matchMedia = query => {"
            "    if (!/min-resolution|min-device-pixel-ratio/.test(query)) return real(query);"
            "    const m = String(query).match(/([0-9.]+)/);"
            "    const want = m ? parseFloat(m[1]) : 1;"
            "    const dppx = /dpi/.test(query) ? want / 96 : want;"
            "    return { media: query, matches: ratio >= dppx, onchange: null,"
            "             addEventListener() {}, removeEventListener() {},"
            "             addListener() {}, removeListener() {},"
            "             dispatchEvent() { return false; } };"
            "  };"
            "})();",
            2);   /* the sheets come in 1x and 2x; anything above 1.25 wants the 2x */
        WebKitUserScript *ratio = webkit_user_script_new(
            hidpi, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
        webkit_user_content_manager_add_script(content, ratio);
        webkit_user_script_unref(ratio);
        g_free(hidpi);
    }

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
    g_signal_connect(self->view, "query-permission-state",
                     G_CALLBACK(on_permission_state_query), self);
    allow_notifications(self->view);
    g_signal_connect(self->view, "notify::title",
                     G_CALLBACK(on_title_changed), self);
    /* Every notification WhatsApp Web raises for itself is dressed and sent on
     * by the app instead -- WebKit's own handler shows a silent banner that
     * lingers in the tray, and it cannot say which chat a click should open. */
    g_signal_connect(self->view, "show-notification",
                     G_CALLBACK(on_show_notification), self);
    g_signal_connect(self->view, "create", G_CALLBACK(on_create_web_view), self);
    g_signal_connect(self->view, "decide-policy", G_CALLBACK(on_decide_policy), self);
    g_signal_connect(self->view, "load-changed", G_CALLBACK(on_load_changed), self);
    webkit_web_view_set_zoom_level(self->view, zoom);

    self->window = GTK_WINDOW(gtk_application_window_new(self->app));
    gtk_window_set_title(self->window, WA_TITLE);
    gtk_window_set_icon_name(self->window, WA_ICON_NAME);
    gtk_window_set_default_size(self->window, width, height);
    gtk_window_set_child(self->window, GTK_WIDGET(self->view));
    g_signal_connect(self->window, "close-request", G_CALLBACK(on_close_request), self);
    g_signal_connect(self->window, "notify::is-active",
                     G_CALLBACK(on_window_state_changed), self);
    g_signal_connect(self->window, "notify::visible",
                     G_CALLBACK(on_window_state_changed), self);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self->window), keys);

    watch_debug_file(self);
    avatars_sweep();

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
on_present_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    show_window(user_data);
}

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    WaApp *self = user_data;
    const gboolean first_run = (self->window == NULL);

    if (first_run) {
        build_window(self);

        /* Clicking one of our notifications should raise the window. */
        GSimpleAction *present = g_simple_action_new("present", NULL);
        g_signal_connect(present, "activate", G_CALLBACK(on_present_action), self);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(present));
        g_object_unref(present);

        static const WaTrayCallbacks callbacks = { on_tray_activate, on_tray_quit };
        self->tray = wa_tray_new(WA_TRAY_ICON, WA_TITLE, WA_APP_ID ".desktop",
                                 &callbacks, self);

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
        /* Answered before GTK is touched, so they work over ssh, in a container,
         * and anywhere else without a display -- which is also what lets a
         * packaging test execute the binary it just installed. */
        if (g_strcmp0(argv[i], "--version") == 0) {
            g_print("%s %s\n", WA_TITLE, WA_VERSION);
            return 0;
        }
        if (g_strcmp0(argv[i], "--help") == 0 || g_strcmp0(argv[i], "-h") == 0) {
            g_print("Usage: whatsapp [--hidden] [--version] [--help]\n\n"
                    "  --hidden   start in the tray without showing a window\n"
                    "  --version  print the version and exit\n"
                    "  --help     print this message and exit\n\n"
                    "Ctrl+V pastes an image, Ctrl +/- zooms, Ctrl+0 resets the zoom,\n"
                    "Ctrl+Q quits. Closing the window leaves it running in the tray.\n\n"
                    "Config: %s/whatsapp/whatsapp.conf\n",
                    g_get_user_config_dir());
            return 0;
        }
        if (g_strcmp0(argv[i], "--hidden") == 0)
            self.start_hidden = TRUE;
        else
            argv[kept++] = argv[i];
    }
    argc = kept;
    argv[argc] = NULL;

    g_set_application_name(WA_TITLE);

    self.config_path = g_build_filename(g_get_user_config_dir(), "whatsapp", "whatsapp.conf", NULL);
    self.app = gtk_application_new(WA_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(self.app, "activate", G_CALLBACK(on_activate), &self);

    /* A --hidden launch must never raise a window, not even the window of an
     * instance that is already running. Activation is how a second launch
     * normally reaches the first one, and that path shows the window -- so with
     * the package's system-wide autostart entry and a per-user one both present,
     * logging in would pop the window open instead of starting in the tray. */
    if (self.start_hidden) {
        GError *error = NULL;
        if (!g_application_register(G_APPLICATION(self.app), NULL, &error)) {
            g_warning("could not register the application: %s",
                      error ? error->message : "unknown");
            g_clear_error(&error);
        } else if (g_application_get_is_remote(G_APPLICATION(self.app))) {
            g_message("already running; leaving the running instance alone");
            g_object_unref(self.app);
            g_free(self.config_path);
            return 0;
        }
    }

    int status = g_application_run(G_APPLICATION(self.app), argc, argv);

    wa_tray_free(self.tray);
    g_object_unref(self.app);
    g_free(self.config_path);
    return status;
}
