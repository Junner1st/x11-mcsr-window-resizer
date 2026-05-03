#include "mc-overlay.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <string.h>

typedef struct {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long input_mode;
    unsigned long status;
} MotifWmHints;

#define MWM_HINTS_DECORATIONS (1L << 1)

static XFixed fixed_from_double(double value) {
    return (XFixed)(value * 65536.0);
}

static void set_atom_list(Display *display, Window window, Atom property, Atom *atoms, int count) {
    XChangeProperty(display,
                    window,
                    property,
                    XA_ATOM,
                    32,
                    PropModeReplace,
                    (unsigned char *)atoms,
                    count);
}

static void set_utf8_property(Display *display,
                              Window window,
                              Atom property,
                              Atom utf8_string,
                              const char *value) {
    XChangeProperty(display,
                    window,
                    property,
                    utf8_string,
                    8,
                    PropModeReplace,
                    (const unsigned char *)value,
                    (int)strlen(value));
}

static void apply_input_shape(McOverlay *overlay) {
    if (!overlay->shape_input_available) {
        return;
    }
    XShapeCombineRectangles(overlay->display,
                            overlay->window,
                            ShapeInput,
                            0,
                            0,
                            NULL,
                            0,
                            ShapeSet,
                            Unsorted);
}

static void apply_window_state(McOverlay *overlay) {
    Atom state_atoms[] = {
        overlay->net_wm_state_above,
        overlay->net_wm_state_skip_taskbar,
        overlay->net_wm_state_skip_pager,
        overlay->net_wm_state_sticky,
    };
    set_atom_list(overlay->display,
                  overlay->window,
                  overlay->net_wm_state,
                  state_atoms,
                  (int)(sizeof(state_atoms) / sizeof(state_atoms[0])));

    Atom type_atoms[] = {overlay->net_wm_window_type_utility};
    set_atom_list(overlay->display,
                  overlay->window,
                  overlay->net_wm_window_type,
                  type_atoms,
                  (int)(sizeof(type_atoms) / sizeof(type_atoms[0])));
}

static bool create_window(McOverlay *overlay, int x, int y, unsigned int w, unsigned int h) {
    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    attrs.background_pixel = BlackPixel(overlay->display, overlay->screen);
    attrs.event_mask = ExposureMask | StructureNotifyMask;

    overlay->window = XCreateWindow(overlay->display,
                                    overlay->root,
                                    x,
                                    y,
                                    w,
                                    h,
                                    0,
                                    CopyFromParent,
                                    InputOutput,
                                    CopyFromParent,
                                    CWOverrideRedirect | CWBackPixel | CWEventMask,
                                    &attrs);
    if (overlay->window == None) {
        return false;
    }

    XStoreName(overlay->display, overlay->window, "mc-resizer-overlay");
    set_utf8_property(overlay->display,
                      overlay->window,
                      overlay->net_wm_name,
                      overlay->utf8_string,
                      "mc-resizer-overlay");

    XClassHint class_hint;
    class_hint.res_name = "mc-resizer-overlay";
    class_hint.res_class = "McResizerOverlay";
    XSetClassHint(overlay->display, overlay->window, &class_hint);

    XWMHints wm_hints;
    memset(&wm_hints, 0, sizeof(wm_hints));
    wm_hints.flags = InputHint;
    wm_hints.input = False;
    XSetWMHints(overlay->display, overlay->window, &wm_hints);

    MotifWmHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = MWM_HINTS_DECORATIONS;
    hints.decorations = 0;
    XChangeProperty(overlay->display,
                    overlay->window,
                    overlay->motif_wm_hints,
                    overlay->motif_wm_hints,
                    32,
                    PropModeReplace,
                    (unsigned char *)&hints,
                    5);

    apply_window_state(overlay);
    apply_input_shape(overlay);

    XRenderPictFormat *format =
        XRenderFindVisualFormat(overlay->display, DefaultVisual(overlay->display, overlay->screen));
    if (format) {
        overlay->picture = XRenderCreatePicture(overlay->display, overlay->window, format, 0, NULL);
    }

    return true;
}

bool mc_overlay_init(McOverlay *overlay, Display *display, int screen, Window root) {
    memset(overlay, 0, sizeof(*overlay));
    overlay->display = display;
    overlay->screen = screen;
    overlay->root = root;
    overlay->window = None;
    overlay->picture = None;

    overlay->net_wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    overlay->net_wm_state_above = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
    overlay->net_wm_state_skip_taskbar = XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", False);
    overlay->net_wm_state_skip_pager = XInternAtom(display, "_NET_WM_STATE_SKIP_PAGER", False);
    overlay->net_wm_state_sticky = XInternAtom(display, "_NET_WM_STATE_STICKY", False);
    overlay->net_wm_window_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    overlay->net_wm_window_type_utility = XInternAtom(display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    overlay->net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    overlay->utf8_string = XInternAtom(display, "UTF8_STRING", False);
    overlay->motif_wm_hints = XInternAtom(display, "_MOTIF_WM_HINTS", False);

    int event_base = 0;
    int error_base = 0;
    overlay->shape_input_available = XShapeQueryExtension(display, &event_base, &error_base) != 0;
    (void)event_base;
    (void)error_base;
    return true;
}

void mc_overlay_destroy(McOverlay *overlay) {
    if (overlay->picture != None) {
        XRenderFreePicture(overlay->display, overlay->picture);
        overlay->picture = None;
    }
    if (overlay->window != None) {
        XDestroyWindow(overlay->display, overlay->window);
        overlay->window = None;
    }
    overlay->visible = false;
}

bool mc_overlay_show(McOverlay *overlay, int x, int y, unsigned int w, unsigned int h) {
    if (w == 0 || h == 0) {
        return false;
    }
    if (overlay->window == None && !create_window(overlay, x, y, w, h)) {
        return false;
    }

    overlay->x = x;
    overlay->y = y;
    overlay->w = w;
    overlay->h = h;

    XMoveResizeWindow(overlay->display, overlay->window, x, y, w, h);
    apply_window_state(overlay);
    apply_input_shape(overlay);
    XMapRaised(overlay->display, overlay->window);
    XRaiseWindow(overlay->display, overlay->window);
    apply_window_state(overlay);
    overlay->visible = true;
    return true;
}

void mc_overlay_hide(McOverlay *overlay) {
    if (overlay->window != None) {
        XUnmapWindow(overlay->display, overlay->window);
        XFlush(overlay->display);
    }
    overlay->visible = false;
}

bool mc_overlay_visible(const McOverlay *overlay) {
    return overlay->visible && overlay->window != None;
}

Window mc_overlay_window(const McOverlay *overlay) {
    return overlay->window;
}

bool mc_overlay_paint(McOverlay *overlay, Window source, const McCenteringRect *source_rect) {
    if (!mc_overlay_visible(overlay) || source == None || !source_rect ||
        source_rect->w == 0 || source_rect->h == 0 || overlay->picture == None) {
        return false;
    }

    XRenderPictFormat *format =
        XRenderFindVisualFormat(overlay->display, DefaultVisual(overlay->display, overlay->screen));
    if (!format) {
        return false;
    }

    Picture source_picture = XRenderCreatePicture(overlay->display, source, format, 0, NULL);
    if (source_picture == None) {
        return false;
    }

    double scale_x = (double)source_rect->w / (double)overlay->w;
    double scale_y = (double)source_rect->h / (double)overlay->h;
    XTransform transform = {{
        {fixed_from_double(scale_x), fixed_from_double(0.0), fixed_from_double((double)source_rect->x)},
        {fixed_from_double(0.0), fixed_from_double(scale_y), fixed_from_double((double)source_rect->y)},
        {fixed_from_double(0.0), fixed_from_double(0.0), fixed_from_double(1.0)},
    }};
    XRenderSetPictureTransform(overlay->display, source_picture, &transform);
    XRenderSetPictureFilter(overlay->display, source_picture, FilterBilinear, NULL, 0);
    XRenderComposite(overlay->display,
                     PictOpSrc,
                     source_picture,
                     None,
                     overlay->picture,
                     0,
                     0,
                     0,
                     0,
                     0,
                     0,
                     overlay->w,
                     overlay->h);
    XRenderFreePicture(overlay->display, source_picture);
    XFlush(overlay->display);
    return true;
}

void mc_overlay_raise(McOverlay *overlay) {
    if (mc_overlay_visible(overlay)) {
        XRaiseWindow(overlay->display, overlay->window);
    }
}
