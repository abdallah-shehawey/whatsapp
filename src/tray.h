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
                    const WaTrayCallbacks *callbacks,
                    gpointer user_data);

/* Switches the item to NeedsAttention, which is what makes a host swap in the
 * attention icon. Cheap to call repeatedly: unchanged states are ignored. */
void wa_tray_set_attention(WaTray *tray, gboolean needs_attention);

void wa_tray_free(WaTray *tray);
