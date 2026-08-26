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

/* Publishes the unread count two ways: the tray item goes to NeedsAttention so
 * the host swaps in the attention icon, and a Unity LauncherEntry signal puts a
 * numbered badge on the dock icon. Cheap to call repeatedly -- an unchanged
 * count does nothing. */
void wa_tray_set_unread(WaTray *tray, int count);

void wa_tray_free(WaTray *tray);
