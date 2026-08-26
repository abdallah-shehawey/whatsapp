/* Minimal StatusNotifierItem tray icon, spoken directly over D-Bus. */
#pragma once

#include <glib.h>

typedef struct WaTray WaTray;

typedef struct {
    void (*activate)(gpointer user_data);   /* left click / "Open" */
    void (*quit)(gpointer user_data);       /* "Quit" */
} WaTrayCallbacks;

WaTray *wa_tray_new(const char *icon_name,
                    const char *title,
                    const char *desktop_id,
                    const WaTrayCallbacks *callbacks,
                    gpointer user_data);

/* Marks the tray item NeedsAttention so the host swaps in the attention icon.
 * No number is published anywhere: the app used to put the unread count on the
 * dock icon over com.canonical.Unity.LauncherEntry, and the badge was more
 * noise than news. Cheap to call repeatedly -- an unchanged state does nothing. */
void wa_tray_set_attention(WaTray *tray, gboolean unread);

void wa_tray_free(WaTray *tray);
