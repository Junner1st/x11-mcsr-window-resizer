#ifndef MC_OVERLAY_H
#define MC_OVERLAY_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <stdbool.h>

#include "mc-centering.h"

typedef struct {
    Display *display;
    int screen;
    Window root;
    Window window;
    Picture picture;
    Atom net_wm_state;
    Atom net_wm_state_above;
    Atom net_wm_state_skip_taskbar;
    Atom net_wm_state_skip_pager;
    Atom net_wm_state_sticky;
    Atom net_wm_window_type;
    Atom net_wm_window_type_utility;
    Atom net_wm_name;
    Atom utf8_string;
    Atom motif_wm_hints;
    int x;
    int y;
    unsigned int w;
    unsigned int h;
    bool visible;
    bool shape_input_available;
} McOverlay;

bool mc_overlay_init(McOverlay *overlay, Display *display, int screen, Window root);
void mc_overlay_destroy(McOverlay *overlay);
bool mc_overlay_show(McOverlay *overlay, int x, int y, unsigned int w, unsigned int h);
void mc_overlay_hide(McOverlay *overlay);
bool mc_overlay_visible(const McOverlay *overlay);
Window mc_overlay_window(const McOverlay *overlay);
bool mc_overlay_paint(McOverlay *overlay, Window source, const McCenteringRect *source_rect);
void mc_overlay_raise(McOverlay *overlay);

#endif
