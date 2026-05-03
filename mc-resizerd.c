#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX 108
#endif

#define MAX_PATTERNS 16
#define MAX_BINDINGS 4
#define GEOMETRY_TOLERANCE 80

typedef enum {
    MODE_UNKNOWN = 0,
    MODE_FULL,
    MODE_THIN,
    MODE_WIDE
} Mode;

typedef enum {
    BACKEND_EWMH = 0,
    BACKEND_FRAME_DIRECT
} Backend;

typedef enum {
    REFRESH_NONE = 0,
    REFRESH_EXPOSE,
    REFRESH_FOCUS_BOUNCE
} RefreshStrategy;

typedef enum {
    CMD_NONE = 0,
    CMD_THIN,
    CMD_WIDE,
    CMD_FULL,
    CMD_CYCLE
} Command;

typedef struct {
    int monitor_x;
    int monitor_y;
    unsigned int monitor_w;
    unsigned int monitor_h;
    unsigned int thin_w;
    unsigned int thin_h;
    unsigned int wide_w;
    unsigned int wide_h;
    Backend backend;
    RefreshStrategy refresh;
    int refresh_delay_ms;
    int settle_timeout_ms;
    char socket_path[UNIX_PATH_MAX];
    char key_thin[64];
    char key_wide[64];
    char key_full[64];
    char key_cycle[64];
    char *title_patterns[MAX_PATTERNS];
    int title_pattern_count;
    char *class_patterns[MAX_PATTERNS];
    int class_pattern_count;
} Config;

typedef struct {
    const char *name;
    Command command;
    char spec[64];
    KeyCode keycode;
    unsigned int modifiers;
} Binding;

typedef struct {
    Display *display;
    int screen;
    Window root;
    Window target;
    Window frame;
    Mode current_mode;
    int last_x;
    int last_y;
    unsigned int last_w;
    unsigned int last_h;
    Atom net_client_list;
    Atom net_wm_name;
    Atom utf8_string;
    Atom net_wm_state;
    Atom net_wm_state_fullscreen;
    Atom net_wm_state_hidden;
    Atom net_active_window;
    Atom net_moveresize_window;
    Atom net_current_desktop;
    Atom net_wm_desktop;
    Atom wm_state;
    unsigned int numlock_mask;
    Binding bindings[MAX_BINDINGS];
    int binding_count;
    Config config;
    int listen_fd;
    bool verbose;
    int x_error_count;
} App;

static void handle_x_event(App *app, XEvent *event);

static App *error_app = NULL;

static volatile sig_atomic_t running = 1;

static void on_signal(int signo) {
    (void)signo;
    running = 0;
}

static void log_msg(const char *fmt, ...) {
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);

    fprintf(stderr, "[%s] ", stamp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int x_error_handler(Display *display, XErrorEvent *event) {
    char buf[256];
    XGetErrorText(display, event->error_code, buf, sizeof(buf));
    log_msg("X11 error: %s (request=%u minor=%u resource=0x%lx)",
            buf,
            event->request_code,
            event->minor_code,
            event->resourceid);
    if (error_app) {
        error_app->x_error_count++;
    }
    return 0;
}

static char *xstrdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (!copy) {
        perror("malloc");
        exit(1);
    }
    memcpy(copy, s, len);
    return copy;
}

static void trim(char *s) {
    char *start = s;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void lower_copy(char *dst, size_t dst_len, const char *src) {
    size_t i;
    for (i = 0; i + 1 < dst_len && src[i]; i++) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static bool contains_casefold(const char *haystack, const char *needle) {
    if (!needle || !needle[0]) {
        return true;
    }

    char h[1024];
    char n[128];
    lower_copy(h, sizeof(h), haystack ? haystack : "");
    lower_copy(n, sizeof(n), needle);
    return strstr(h, n) != NULL;
}

static void add_pattern(char **patterns, int *count, const char *value) {
    if (*count >= MAX_PATTERNS || !value || !value[0]) {
        return;
    }
    patterns[*count] = xstrdup(value);
    (*count)++;
}

static void split_patterns(char **patterns, int *count, const char *value) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", value);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        trim(tok);
        add_pattern(patterns, count, tok);
    }
}

static void default_socket_path(char *out, size_t out_len) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0]) {
        snprintf(out, out_len, "%s/mc-resizer.sock", runtime);
    } else {
        snprintf(out, out_len, "/tmp/mc-resizer-%ld.sock", (long)getuid());
    }
}

static void config_defaults(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->monitor_x = 0;
    cfg->monitor_y = 0;
    cfg->monitor_w = 2560;
    cfg->monitor_h = 1440;
    cfg->thin_w = 2560;
    cfg->thin_h = 267;
    cfg->wide_w = 600;
    cfg->wide_h = 1440;
    cfg->backend = BACKEND_EWMH;
    cfg->refresh = REFRESH_FOCUS_BOUNCE;
    cfg->refresh_delay_ms = 35;
    cfg->settle_timeout_ms = 80;
    default_socket_path(cfg->socket_path, sizeof(cfg->socket_path));
    snprintf(cfg->key_thin, sizeof(cfg->key_thin), "%s", "Ctrl+Alt+1");
    snprintf(cfg->key_wide, sizeof(cfg->key_wide), "%s", "Ctrl+Alt+2");
    snprintf(cfg->key_full, sizeof(cfg->key_full), "%s", "Ctrl+Alt+3");
    snprintf(cfg->key_cycle, sizeof(cfg->key_cycle), "%s", "Ctrl+Alt+4");
    split_patterns(cfg->title_patterns, &cfg->title_pattern_count, "Minecraft,MCSR");
    split_patterns(cfg->class_patterns, &cfg->class_pattern_count, "Minecraft,GLFW,java");
}

static void clear_patterns(Config *cfg, bool title) {
    char **patterns = title ? cfg->title_patterns : cfg->class_patterns;
    int *count = title ? &cfg->title_pattern_count : &cfg->class_pattern_count;
    for (int i = 0; i < *count; i++) {
        free(patterns[i]);
        patterns[i] = NULL;
    }
    *count = 0;
}

static void load_config(Config *cfg, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errno != ENOENT) {
            log_msg("could not read config %s: %s", path, strerror(errno));
        }
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        trim(line);
        if (!line[0]) {
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim(key);
        trim(value);

        if (strcmp(key, "monitor_x") == 0) {
            cfg->monitor_x = atoi(value);
        } else if (strcmp(key, "monitor_y") == 0) {
            cfg->monitor_y = atoi(value);
        } else if (strcmp(key, "monitor_w") == 0) {
            cfg->monitor_w = (unsigned int)strtoul(value, NULL, 10);
        } else if (strcmp(key, "monitor_h") == 0) {
            cfg->monitor_h = (unsigned int)strtoul(value, NULL, 10);
        } else if (strcmp(key, "thin_w") == 0) {
            cfg->thin_w = (unsigned int)strtoul(value, NULL, 10);
        } else if (strcmp(key, "thin_h") == 0) {
            cfg->thin_h = (unsigned int)strtoul(value, NULL, 10);
        } else if (strcmp(key, "wide_w") == 0) {
            cfg->wide_w = (unsigned int)strtoul(value, NULL, 10);
        } else if (strcmp(key, "wide_h") == 0) {
            cfg->wide_h = (unsigned int)strtoul(value, NULL, 10);
        } else if (strcmp(key, "backend") == 0) {
            if (strcmp(value, "frame-direct") == 0) {
                cfg->backend = BACKEND_FRAME_DIRECT;
            } else {
                cfg->backend = BACKEND_EWMH;
            }
        } else if (strcmp(key, "refresh") == 0) {
            if (strcmp(value, "none") == 0) {
                cfg->refresh = REFRESH_NONE;
            } else if (strcmp(value, "expose") == 0) {
                cfg->refresh = REFRESH_EXPOSE;
            } else {
                cfg->refresh = REFRESH_FOCUS_BOUNCE;
            }
        } else if (strcmp(key, "refresh_delay_ms") == 0) {
            cfg->refresh_delay_ms = atoi(value);
            if (cfg->refresh_delay_ms < 0) {
                cfg->refresh_delay_ms = 0;
            }
        } else if (strcmp(key, "settle_timeout_ms") == 0) {
            cfg->settle_timeout_ms = atoi(value);
            if (cfg->settle_timeout_ms < 0) {
                cfg->settle_timeout_ms = 0;
            }
        } else if (strcmp(key, "socket_path") == 0) {
            if (value[0]) {
                snprintf(cfg->socket_path, sizeof(cfg->socket_path), "%s", value);
            }
        } else if (strcmp(key, "key_thin") == 0) {
            snprintf(cfg->key_thin, sizeof(cfg->key_thin), "%s", value);
        } else if (strcmp(key, "key_wide") == 0) {
            snprintf(cfg->key_wide, sizeof(cfg->key_wide), "%s", value);
        } else if (strcmp(key, "key_full") == 0) {
            snprintf(cfg->key_full, sizeof(cfg->key_full), "%s", value);
        } else if (strcmp(key, "key_cycle") == 0) {
            snprintf(cfg->key_cycle, sizeof(cfg->key_cycle), "%s", value);
        } else if (strcmp(key, "title_match") == 0) {
            clear_patterns(cfg, true);
            split_patterns(cfg->title_patterns, &cfg->title_pattern_count, value);
        } else if (strcmp(key, "class_match") == 0) {
            clear_patterns(cfg, false);
            split_patterns(cfg->class_patterns, &cfg->class_pattern_count, value);
        }
    }

    fclose(fp);
}

static const char *mode_name(Mode mode) {
    switch (mode) {
    case MODE_FULL:
        return "full";
    case MODE_THIN:
        return "thin";
    case MODE_WIDE:
        return "wide";
    default:
        return "unknown";
    }
}

static bool near_size(unsigned int w, unsigned int h, unsigned int tw, unsigned int th) {
    int dw = (int)w - (int)tw;
    int dh = (int)h - (int)th;
    if (dw < 0) {
        dw = -dw;
    }
    if (dh < 0) {
        dh = -dh;
    }
    return dw <= GEOMETRY_TOLERANCE && dh <= GEOMETRY_TOLERANCE;
}

static long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void sleep_ms(int ms) {
    if (ms <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

static Mode detect_mode(App *app, unsigned int w, unsigned int h) {
    Config *cfg = &app->config;
    if (near_size(w, h, cfg->monitor_w, cfg->monitor_h)) {
        return MODE_FULL;
    }
    if (near_size(w, h, cfg->thin_w, cfg->thin_h)) {
        return MODE_THIN;
    }
    if (near_size(w, h, cfg->wide_w, cfg->wide_h)) {
        return MODE_WIDE;
    }
    return MODE_UNKNOWN;
}

static char *get_text_property(Display *display, Window window, Atom atom) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *prop = NULL;

    int rc = XGetWindowProperty(display,
                                window,
                                atom,
                                0,
                                4096,
                                False,
                                AnyPropertyType,
                                &actual_type,
                                &actual_format,
                                &nitems,
                                &bytes_after,
                                &prop);
    if (rc != Success || !prop) {
        return NULL;
    }

    (void)actual_type;
    (void)actual_format;
    (void)bytes_after;

    char *out = calloc(nitems + 1, 1);
    if (!out) {
        XFree(prop);
        return NULL;
    }
    memcpy(out, prop, nitems);
    XFree(prop);
    return out;
}

static char *get_window_title(App *app, Window window) {
    char *title = get_text_property(app->display, window, app->net_wm_name);
    if (title && title[0]) {
        return title;
    }
    free(title);

    char *fallback = NULL;
    if (XFetchName(app->display, window, &fallback) && fallback) {
        title = xstrdup(fallback);
        XFree(fallback);
        return title;
    }
    return xstrdup("");
}

static char *get_window_class(Display *display, Window window) {
    XClassHint hint;
    if (!XGetClassHint(display, window, &hint)) {
        return xstrdup("");
    }

    char buf[512];
    snprintf(buf,
             sizeof(buf),
             "%s %s",
             hint.res_name ? hint.res_name : "",
             hint.res_class ? hint.res_class : "");
    if (hint.res_name) {
        XFree(hint.res_name);
    }
    if (hint.res_class) {
        XFree(hint.res_class);
    }
    return xstrdup(buf);
}

static int pattern_score(char **patterns, int pattern_count, const char *value, int weight) {
    for (int i = 0; i < pattern_count; i++) {
        if (contains_casefold(value, patterns[i])) {
            return weight;
        }
    }
    return 0;
}

static int score_window(App *app, Window window, char **title_out, char **class_out) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(app->display, window, &attr)) {
        return 0;
    }
    if (attr.map_state != IsViewable) {
        return 0;
    }

    char *title = get_window_title(app, window);
    char *class_name = get_window_class(app->display, window);
    int title_score = pattern_score(app->config.title_patterns,
                                    app->config.title_pattern_count,
                                    title,
                                    6);
    int class_score = pattern_score(app->config.class_patterns,
                                    app->config.class_pattern_count,
                                    class_name,
                                    3);

    int score = 0;
    if (title_score > 0 && class_score > 0) {
        score = title_score + class_score;
    }
    if (score > 0 && title && title[0]) {
        score += 1;
    }

    if (title_out) {
        *title_out = title;
    } else {
        free(title);
    }
    if (class_out) {
        *class_out = class_name;
    } else {
        free(class_name);
    }
    return score;
}

static bool read_window_list(App *app, Window **windows_out, unsigned long *count_out) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *prop = NULL;

    int rc = XGetWindowProperty(app->display,
                                app->root,
                                app->net_client_list,
                                0,
                                4096,
                                False,
                                XA_WINDOW,
                                &actual_type,
                                &actual_format,
                                &nitems,
                                &bytes_after,
                                &prop);
    if (rc != Success || !prop || actual_type != XA_WINDOW || actual_format != 32) {
        if (prop) {
            XFree(prop);
        }
        return false;
    }

    (void)bytes_after;
    *windows_out = (Window *)prop;
    *count_out = nitems;
    return true;
}

static Window direct_child_of_root(App *app, Window window) {
    Window root_return;
    Window parent;
    Window *children = NULL;
    unsigned int nchildren = 0;
    Window current = window;

    while (current != None && current != app->root) {
        if (!XQueryTree(app->display, current, &root_return, &parent, &children, &nchildren)) {
            if (children) {
                XFree(children);
            }
            return window;
        }
        if (children) {
            XFree(children);
            children = NULL;
        }
        if (parent == app->root) {
            return current;
        }
        current = parent;
    }
    return window;
}

static bool update_geometry(App *app) {
    if (app->target == None) {
        return false;
    }

    Window geom_window = app->config.backend == BACKEND_FRAME_DIRECT && app->frame != None
                             ? app->frame
                             : app->target;
    XWindowAttributes attr;
    if (!XGetWindowAttributes(app->display, geom_window, &attr)) {
        return false;
    }

    Window child;
    int root_x = 0;
    int root_y = 0;
    XTranslateCoordinates(app->display,
                          geom_window,
                          app->root,
                          0,
                          0,
                          &root_x,
                          &root_y,
                          &child);

    app->last_x = root_x;
    app->last_y = root_y;
    app->last_w = (unsigned int)attr.width;
    app->last_h = (unsigned int)attr.height;
    app->current_mode = detect_mode(app, app->last_w, app->last_h);
    return true;
}

static bool attach_window(App *app, Window window, const char *title, const char *class_name) {
    app->target = window;
    app->frame = direct_child_of_root(app, window);
    XSelectInput(app->display, app->target, StructureNotifyMask | PropertyChangeMask);
    if (app->frame != app->target) {
        XSelectInput(app->display, app->frame, StructureNotifyMask | PropertyChangeMask);
    }
    update_geometry(app);
    log_msg("attached window=0x%lx frame=0x%lx mode=%s title=\"%s\" class=\"%s\"",
            app->target,
            app->frame,
            mode_name(app->current_mode),
            title ? title : "",
            class_name ? class_name : "");
    return true;
}

static bool find_target_window(App *app) {
    Window *windows = NULL;
    unsigned long count = 0;
    bool from_client_list = read_window_list(app, &windows, &count);

    Window *fallback_children = NULL;
    if (!from_client_list) {
        Window root_return;
        Window parent_return;
        unsigned int nchildren = 0;
        if (!XQueryTree(app->display,
                        app->root,
                        &root_return,
                        &parent_return,
                        &fallback_children,
                        &nchildren)) {
            return false;
        }
        windows = fallback_children;
        count = nchildren;
    }

    Window best = None;
    int best_score = 0;
    char *best_title = NULL;
    char *best_class = NULL;

    for (unsigned long i = 0; i < count; i++) {
        char *title = NULL;
        char *class_name = NULL;
        int score = score_window(app, windows[i], &title, &class_name);
        if (score > best_score) {
            free(best_title);
            free(best_class);
            best = windows[i];
            best_score = score;
            best_title = title;
            best_class = class_name;
        } else {
            free(title);
            free(class_name);
        }
    }

    if (windows) {
        XFree(windows);
    }

    if (best != None && best_score > 0) {
        attach_window(app, best, best_title, best_class);
        free(best_title);
        free(best_class);
        return true;
    }

    free(best_title);
    free(best_class);
    return false;
}

static bool ensure_target(App *app) {
    if (app->target != None) {
        XWindowAttributes attr;
        if (XGetWindowAttributes(app->display, app->target, &attr)) {
            return true;
        }
        app->target = None;
        app->frame = None;
        app->current_mode = MODE_UNKNOWN;
    }
    return find_target_window(app);
}

static void target_rect(App *app, Mode mode, int *x, int *y, unsigned int *w, unsigned int *h) {
    Config *cfg = &app->config;
    switch (mode) {
    case MODE_THIN:
        *w = cfg->thin_w;
        *h = cfg->thin_h;
        break;
    case MODE_WIDE:
        *w = cfg->wide_w;
        *h = cfg->wide_h;
        break;
    case MODE_FULL:
    default:
        *w = cfg->monitor_w;
        *h = cfg->monitor_h;
        break;
    }

    *x = cfg->monitor_x + ((int)cfg->monitor_w - (int)*w) / 2;
    *y = cfg->monitor_y + ((int)cfg->monitor_h - (int)*h) / 2;
}

static bool resize_ewmh(App *app, int x, int y, unsigned int w, unsigned int h) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.serial = 0;
    ev.xclient.send_event = True;
    ev.xclient.display = app->display;
    ev.xclient.window = app->target;
    ev.xclient.message_type = app->net_moveresize_window;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = StaticGravity | (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11);
    ev.xclient.data.l[1] = x;
    ev.xclient.data.l[2] = y;
    ev.xclient.data.l[3] = (long)w;
    ev.xclient.data.l[4] = (long)h;

    return XSendEvent(app->display,
                      app->root,
                      False,
                      SubstructureRedirectMask | SubstructureNotifyMask,
                      &ev) != 0;
}

static bool remove_fullscreen(App *app) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.serial = 0;
    ev.xclient.send_event = True;
    ev.xclient.display = app->display;
    ev.xclient.window = app->target;
    ev.xclient.message_type = app->net_wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 0; /* _NET_WM_STATE_REMOVE */
    ev.xclient.data.l[1] = (long)app->net_wm_state_fullscreen;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 1; /* application source indication */
    ev.xclient.data.l[4] = 0;

    return XSendEvent(app->display,
                      app->root,
                      False,
                      SubstructureRedirectMask | SubstructureNotifyMask,
                      &ev) != 0;
}

static bool activate_window(App *app, Window window) {
    if (window == None) {
        return false;
    }
    XMapRaised(app->display, window);

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.serial = 0;
    ev.xclient.send_event = True;
    ev.xclient.display = app->display;
    ev.xclient.window = window;
    ev.xclient.message_type = app->net_active_window;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 1; /* application source indication */
    ev.xclient.data.l[1] = CurrentTime;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 0;
    ev.xclient.data.l[4] = 0;

    return XSendEvent(app->display,
                      app->root,
                      False,
                      SubstructureRedirectMask | SubstructureNotifyMask,
                      &ev) != 0;
}

static bool focus_window_direct(App *app, Window window) {
    if (window == None) {
        return false;
    }

    XWindowAttributes attr;
    if (!XGetWindowAttributes(app->display, window, &attr) ||
        attr.map_state != IsViewable ||
        attr.class != InputOutput) {
        return false;
    }

    XSetInputFocus(app->display, window, RevertToPointerRoot, CurrentTime);
    return true;
}

static bool window_has_atom_property(App *app, Window window, Atom property, Atom needle) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *prop = NULL;

    int rc = XGetWindowProperty(app->display,
                                window,
                                property,
                                0,
                                64,
                                False,
                                XA_ATOM,
                                &actual_type,
                                &actual_format,
                                &nitems,
                                &bytes_after,
                                &prop);
    if (rc != Success || !prop || actual_type != XA_ATOM || actual_format != 32) {
        if (prop) {
            XFree(prop);
        }
        return false;
    }

    Atom *atoms = (Atom *)prop;
    bool found = false;
    for (unsigned long i = 0; i < nitems; i++) {
        if (atoms[i] == needle) {
            found = true;
            break;
        }
    }
    XFree(prop);
    (void)bytes_after;
    return found;
}

static int get_window_wm_state(App *app, Window window) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *prop = NULL;

    int rc = XGetWindowProperty(app->display,
                                window,
                                app->wm_state,
                                0,
                                2,
                                False,
                                app->wm_state,
                                &actual_type,
                                &actual_format,
                                &nitems,
                                &bytes_after,
                                &prop);
    if (rc != Success || !prop || actual_format != 32 || nitems < 1) {
        if (prop) {
            XFree(prop);
        }
        return NormalState;
    }

    long state = ((long *)prop)[0];
    XFree(prop);
    (void)actual_type;
    (void)bytes_after;
    return (int)state;
}

static int get_wm_state(App *app) {
    return get_window_wm_state(app, app->target);
}

static bool get_cardinal_property(App *app, Window window, Atom property, unsigned long *value) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *prop = NULL;

    int rc = XGetWindowProperty(app->display,
                                window,
                                property,
                                0,
                                1,
                                False,
                                XA_CARDINAL,
                                &actual_type,
                                &actual_format,
                                &nitems,
                                &bytes_after,
                                &prop);
    if (rc != Success || !prop || actual_type != XA_CARDINAL || actual_format != 32 || nitems < 1) {
        if (prop) {
            XFree(prop);
        }
        return false;
    }

    *value = ((unsigned long *)prop)[0];
    XFree(prop);
    (void)bytes_after;
    return true;
}

static void ensure_window_mapped(App *app) {
    if (get_wm_state(app) == IconicState ||
        window_has_atom_property(app, app->target, app->net_wm_state, app->net_wm_state_hidden)) {
        activate_window(app, app->target);
        XFlush(app->display);
        sleep_ms(app->config.refresh_delay_ms);
    }
}

static void wait_for_fullscreen_removed(App *app) {
    int timeout = app->config.settle_timeout_ms;
    if (timeout <= 0) {
        return;
    }

    long deadline = monotonic_ms() + timeout;
    while (monotonic_ms() < deadline) {
        while (XPending(app->display)) {
            XEvent event;
            XNextEvent(app->display, &event);
            handle_x_event(app, &event);
        }
        if (!window_has_atom_property(app,
                                      app->target,
                                      app->net_wm_state,
                                      app->net_wm_state_fullscreen)) {
            return;
        }
        sleep_ms(1);
    }
}

static bool resize_frame_direct(App *app, int x, int y, unsigned int w, unsigned int h) {
    Window window = app->frame != None ? app->frame : app->target;
    return XMoveResizeWindow(app->display, window, x, y, w, h) != 0;
}

static bool resize_window(App *app, int x, int y, unsigned int w, unsigned int h) {
    return app->config.backend == BACKEND_FRAME_DIRECT
               ? resize_frame_direct(app, x, y, w, h)
               : resize_ewmh(app, x, y, w, h);
}

static void wait_for_resize_settle(App *app, unsigned int w, unsigned int h) {
    int timeout = app->config.settle_timeout_ms;
    if (timeout <= 0) {
        return;
    }

    long deadline = monotonic_ms() + timeout;
    while (monotonic_ms() < deadline) {
        while (XPending(app->display)) {
            XEvent event;
            XNextEvent(app->display, &event);
            handle_x_event(app, &event);
        }

        if (update_geometry(app) && near_size(app->last_w, app->last_h, w, h)) {
            return;
        }
        sleep_ms(1);
    }
}

static bool window_contains(App *app, Window ancestor, Window child) {
    if (ancestor == None || child == None) {
        return false;
    }
    if (ancestor == child) {
        return true;
    }

    Window root_return;
    Window parent;
    Window *children = NULL;
    unsigned int nchildren = 0;
    Window current = child;

    while (current != None && current != app->root) {
        if (!XQueryTree(app->display, current, &root_return, &parent, &children, &nchildren)) {
            if (children) {
                XFree(children);
            }
            return false;
        }
        if (children) {
            XFree(children);
            children = NULL;
        }
        if (parent == ancestor) {
            return true;
        }
        current = parent;
    }
    return false;
}

static bool target_has_focus(App *app) {
    Window focus = None;
    int revert = 0;
    XGetInputFocus(app->display, &focus, &revert);
    (void)revert;
    if (focus == None || focus == PointerRoot) {
        return false;
    }
    return focus == app->target || focus == app->frame ||
           window_contains(app, app->target, focus) ||
           window_contains(app, app->frame, focus);
}

static bool same_desktop_or_sticky(App *app, Window window) {
    unsigned long current_desktop;
    unsigned long window_desktop;

    if (!get_cardinal_property(app, app->root, app->net_current_desktop, &current_desktop)) {
        return true;
    }
    if (!get_cardinal_property(app, window, app->net_wm_desktop, &window_desktop)) {
        return true;
    }

    return window_desktop == current_desktop || window_desktop == 0xffffffffUL;
}

static bool usable_focus_peer(App *app, Window window) {
    if (window == None || window == app->target || window == app->frame) {
        return false;
    }

    XWindowAttributes attr;
    if (!XGetWindowAttributes(app->display, window, &attr) || attr.map_state != IsViewable) {
        return false;
    }
    if (get_window_wm_state(app, window) == IconicState) {
        return false;
    }
    if (window_has_atom_property(app, window, app->net_wm_state, app->net_wm_state_hidden)) {
        return false;
    }
    if (!same_desktop_or_sticky(app, window)) {
        return false;
    }

    return true;
}

static Window find_focus_peer(App *app) {
    Window *windows = NULL;
    unsigned long count = 0;
    if (!read_window_list(app, &windows, &count)) {
        return None;
    }

    Window peer = None;
    for (unsigned long i = count; i > 0; i--) {
        Window candidate = windows[i - 1];
        if (usable_focus_peer(app, candidate)) {
            peer = candidate;
            break;
        }
    }

    XFree(windows);
    return peer;
}

static void send_expose_refresh(App *app) {
    XClearArea(app->display, app->target, 0, 0, 0, 0, True);
    if (app->frame != None && app->frame != app->target) {
        XClearArea(app->display, app->frame, 0, 0, 0, 0, True);
    }
    XFlush(app->display);
}

static void send_focus_bounce_refresh(App *app) {
    Window peer = find_focus_peer(app);
    if (peer == None) {
        send_expose_refresh(app);
        return;
    }

    bool target_was_focused = target_has_focus(app);
    activate_window(app, peer);
    focus_window_direct(app, peer);
    XFlush(app->display);
    sleep_ms(app->config.refresh_delay_ms);

    activate_window(app, app->target);
    focus_window_direct(app, app->target);
    XFlush(app->display);
    if (target_was_focused) {
        sleep_ms(app->config.refresh_delay_ms);
    }

    send_expose_refresh(app);
    XFlush(app->display);
}

static void post_resize_refresh(App *app) {
    switch (app->config.refresh) {
    case REFRESH_EXPOSE:
        send_expose_refresh(app);
        break;
    case REFRESH_FOCUS_BOUNCE:
        send_focus_bounce_refresh(app);
        break;
    case REFRESH_NONE:
    default:
        break;
    }
}

static bool set_mode(App *app, Mode mode) {
    if (!ensure_target(app)) {
        log_msg("no matching target window");
        return false;
    }

    int x;
    int y;
    unsigned int w;
    unsigned int h;
    target_rect(app, mode, &x, &y, &w, &h);

    ensure_window_mapped(app);
    update_geometry(app);

    if (mode != MODE_FULL) {
        remove_fullscreen(app);
        XFlush(app->display);
        wait_for_fullscreen_removed(app);
    }

    bool ok = resize_window(app, x, y, w, h);
    XFlush(app->display);

    if (ok) {
        wait_for_resize_settle(app, w, h);
        if (mode != MODE_FULL) {
            remove_fullscreen(app);
            XFlush(app->display);
            wait_for_fullscreen_removed(app);
        }
        post_resize_refresh(app);
        app->last_x = x;
        app->last_y = y;
        app->last_w = w;
        app->last_h = h;
        app->current_mode = mode;
        if (app->verbose) {
            log_msg("set %s to %dx%d+%d+%d", mode_name(mode), w, h, x, y);
        }
    }
    return ok;
}

static Command command_from_name(const char *name) {
    if (strcmp(name, "thin") == 0) {
        return CMD_THIN;
    }
    if (strcmp(name, "wide") == 0) {
        return CMD_WIDE;
    }
    if (strcmp(name, "full") == 0) {
        return CMD_FULL;
    }
    if (strcmp(name, "cycle") == 0) {
        return CMD_CYCLE;
    }
    return CMD_NONE;
}

static Mode mode_for_command(App *app, Command command) {
    switch (command) {
    case CMD_THIN:
        return app->current_mode == MODE_THIN ? MODE_FULL : MODE_THIN;
    case CMD_WIDE:
        return app->current_mode == MODE_WIDE ? MODE_FULL : MODE_WIDE;
    case CMD_FULL:
        return MODE_FULL;
    case CMD_CYCLE:
        switch (app->current_mode) {
        case MODE_FULL:
            return MODE_THIN;
        case MODE_THIN:
            return MODE_WIDE;
        case MODE_WIDE:
            return MODE_FULL;
        case MODE_UNKNOWN:
        default:
            return MODE_FULL;
        }
    default:
        return MODE_UNKNOWN;
    }
}

static bool run_command(App *app, Command command) {
    update_geometry(app);
    Mode target = mode_for_command(app, command);
    if (target == MODE_UNKNOWN) {
        return false;
    }
    return set_mode(app, target);
}

static unsigned int find_numlock_mask(Display *display) {
    unsigned int numlock = 0;
    XModifierKeymap *map = XGetModifierMapping(display);
    if (!map) {
        return 0;
    }

    KeyCode numlock_code = XKeysymToKeycode(display, XK_Num_Lock);
    for (int mod = 0; mod < 8; mod++) {
        for (int k = 0; k < map->max_keypermod; k++) {
            KeyCode code = map->modifiermap[mod * map->max_keypermod + k];
            if (code == numlock_code) {
                numlock = (unsigned int)(1 << mod);
            }
        }
    }

    XFreeModifiermap(map);
    return numlock;
}

static bool parse_key_spec(App *app, const char *spec, KeyCode *keycode, unsigned int *modifiers) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", spec);
    trim(buf);
    if (!buf[0]) {
        return false;
    }

    unsigned int mods = 0;
    char *last = NULL;
    char *save = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        trim(tok);
        if (!tok[0]) {
            continue;
        }
        if (strcasecmp(tok, "Ctrl") == 0 || strcasecmp(tok, "Control") == 0) {
            mods |= ControlMask;
        } else if (strcasecmp(tok, "Alt") == 0 || strcasecmp(tok, "Mod1") == 0) {
            mods |= Mod1Mask;
        } else if (strcasecmp(tok, "Shift") == 0) {
            mods |= ShiftMask;
        } else if (strcasecmp(tok, "Super") == 0 || strcasecmp(tok, "Win") == 0 ||
                   strcasecmp(tok, "Mod4") == 0) {
            mods |= Mod4Mask;
        } else if (strcasecmp(tok, "Mod2") == 0) {
            mods |= Mod2Mask;
        } else if (strcasecmp(tok, "Mod3") == 0) {
            mods |= Mod3Mask;
        } else if (strcasecmp(tok, "Mod5") == 0) {
            mods |= Mod5Mask;
        } else {
            last = tok;
        }
    }

    if (!last) {
        return false;
    }

    KeySym sym = XStringToKeysym(last);
    if (sym == NoSymbol && strlen(last) == 1) {
        char one[2] = {(char)tolower((unsigned char)last[0]), '\0'};
        sym = XStringToKeysym(one);
    }
    if (sym == NoSymbol) {
        log_msg("unknown key in hotkey spec: %s", spec);
        return false;
    }

    KeyCode code = XKeysymToKeycode(app->display, sym);
    if (code == 0) {
        log_msg("no keycode for hotkey spec: %s", spec);
        return false;
    }

    *keycode = code;
    *modifiers = mods;
    return true;
}

static void grab_one(Display *display, Window root, KeyCode keycode, unsigned int modifiers) {
    XGrabKey(display, (int)keycode, modifiers, root, False, GrabModeAsync, GrabModeAsync);
}

static void add_binding(App *app, const char *name, Command command, const char *spec) {
    if (app->binding_count >= MAX_BINDINGS || !spec || !spec[0]) {
        return;
    }

    Binding *binding = &app->bindings[app->binding_count];
    memset(binding, 0, sizeof(*binding));
    binding->name = name;
    binding->command = command;
    snprintf(binding->spec, sizeof(binding->spec), "%s", spec);

    if (!parse_key_spec(app, spec, &binding->keycode, &binding->modifiers)) {
        return;
    }

    unsigned int variants[] = {
        0,
        LockMask,
        app->numlock_mask,
        LockMask | app->numlock_mask,
    };
    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        int before = app->x_error_count;
        grab_one(app->display, app->root, binding->keycode, binding->modifiers | variants[i]);
        XSync(app->display, False);
        if (app->x_error_count != before) {
            log_msg("failed to grab %s variant modifiers=0x%x; another client may already own it",
                    spec,
                    binding->modifiers | variants[i]);
        }
    }
    app->binding_count++;
    log_msg("grabbed %s hotkey: %s", name, spec);
}

static void setup_hotkeys(App *app) {
    app->numlock_mask = find_numlock_mask(app->display);
    add_binding(app, "thin", CMD_THIN, app->config.key_thin);
    add_binding(app, "wide", CMD_WIDE, app->config.key_wide);
    add_binding(app, "full", CMD_FULL, app->config.key_full);
    add_binding(app, "cycle", CMD_CYCLE, app->config.key_cycle);
    XSync(app->display, False);
}

static Command command_for_key(App *app, XKeyEvent *event) {
    unsigned int ignored = LockMask | app->numlock_mask;
    unsigned int state = event->state & ~ignored;
    for (int i = 0; i < app->binding_count; i++) {
        Binding *binding = &app->bindings[i];
        if (event->keycode == binding->keycode && state == binding->modifiers) {
            return binding->command;
        }
    }
    return CMD_NONE;
}

static int setup_socket(App *app) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_msg("socket failed: %s", strerror(errno));
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", app->config.socket_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno == EADDRINUSE) {
            int probe = socket(AF_UNIX, SOCK_STREAM, 0);
            if (probe >= 0 && connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                log_msg("socket already in use: %s", app->config.socket_path);
                close(probe);
                close(fd);
                return -1;
            }
            if (probe >= 0) {
                close(probe);
            }
            unlink(app->config.socket_path);
            if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                log_msg("bind failed after unlink: %s", strerror(errno));
                close(fd);
                return -1;
            }
        } else {
            log_msg("bind failed: %s", strerror(errno));
            close(fd);
            return -1;
        }
    }

    chmod(app->config.socket_path, 0600);
    if (listen(fd, 16) < 0) {
        log_msg("listen failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    log_msg("listening on %s", app->config.socket_path);
    return fd;
}

static void reply_fd(int fd, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0) {
        ssize_t ignored = write(fd, buf, (size_t)len);
        (void)ignored;
    }
}

static void handle_client(App *app) {
    int fd = accept(app->listen_fd, NULL, NULL);
    if (fd < 0) {
        return;
    }

    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(fd);
        return;
    }
    buf[n] = '\0';
    char *newline = strchr(buf, '\n');
    if (newline) {
        *newline = '\0';
    }
    trim(buf);

    if (strcmp(buf, "status") == 0) {
        update_geometry(app);
        reply_fd(fd,
                 "window=0x%lx frame=0x%lx mode=%s geometry=%ux%u+%d+%d backend=%s\n",
                 app->target,
                 app->frame,
                 mode_name(app->current_mode),
                 app->last_w,
                 app->last_h,
                 app->last_x,
                 app->last_y,
                 app->config.backend == BACKEND_FRAME_DIRECT ? "frame-direct" : "ewmh");
    } else if (strcmp(buf, "rescan") == 0) {
        app->target = None;
        app->frame = None;
        bool ok = find_target_window(app);
        reply_fd(fd, "%s\n", ok ? "ok" : "not-found");
    } else if (strcmp(buf, "quit") == 0) {
        running = 0;
        reply_fd(fd, "ok\n");
    } else {
        Command command = command_from_name(buf);
        if (command == CMD_NONE) {
            reply_fd(fd, "error unknown command\n");
        } else {
            bool ok = run_command(app, command);
            reply_fd(fd, "%s\n", ok ? "ok" : "error no-window-or-resize-failed");
        }
    }

    close(fd);
}

static void clear_target(App *app) {
    if (app->target != None) {
        log_msg("window disappeared: 0x%lx", app->target);
    }
    app->target = None;
    app->frame = None;
    app->current_mode = MODE_UNKNOWN;
}

static void handle_x_event(App *app, XEvent *event) {
    switch (event->type) {
    case KeyPress: {
        Command command = command_for_key(app, &event->xkey);
        if (command != CMD_NONE) {
            run_command(app, command);
        }
        break;
    }
    case MapNotify:
        if (app->target == None) {
            find_target_window(app);
        }
        break;
    case DestroyNotify:
        if (event->xdestroywindow.window == app->target || event->xdestroywindow.window == app->frame) {
            clear_target(app);
        }
        break;
    case ConfigureNotify:
        if (event->xconfigure.window == app->target || event->xconfigure.window == app->frame) {
            update_geometry(app);
        }
        break;
    case PropertyNotify:
        if (event->xproperty.window == app->target) {
            update_geometry(app);
        } else if (app->target == None && event->xproperty.window == app->root) {
            find_target_window(app);
        }
        break;
    default:
        break;
    }
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [--config PATH] [--socket PATH] [--backend ewmh|frame-direct] [--verbose]\n",
            argv0);
}

int main(int argc, char **argv) {
    const char *config_path = "mc-resizer.conf";
    char socket_override[UNIX_PATH_MAX] = "";
    int backend_override = -1;
    App app;
    memset(&app, 0, sizeof(app));
    app.target = None;
    app.frame = None;
    app.current_mode = MODE_UNKNOWN;
    app.listen_fd = -1;
    config_defaults(&app.config);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 2;
            }
            config_path = argv[i];
        } else if (strcmp(argv[i], "--socket") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 2;
            }
            snprintf(socket_override, sizeof(socket_override), "%s", argv[i]);
        } else if (strcmp(argv[i], "--backend") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 2;
            }
            backend_override = strcmp(argv[i], "frame-direct") == 0 ? BACKEND_FRAME_DIRECT : BACKEND_EWMH;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            app.verbose = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    load_config(&app.config, config_path);
    if (socket_override[0]) {
        snprintf(app.config.socket_path, sizeof(app.config.socket_path), "%s", socket_override);
    }
    if (backend_override >= 0) {
        app.config.backend = backend_override == BACKEND_FRAME_DIRECT ? BACKEND_FRAME_DIRECT : BACKEND_EWMH;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    error_app = &app;
    XSetErrorHandler(x_error_handler);

    app.display = XOpenDisplay(NULL);
    if (!app.display) {
        log_msg("could not open X display; check DISPLAY and XAUTHORITY");
        return 1;
    }

    app.screen = DefaultScreen(app.display);
    app.root = RootWindow(app.display, app.screen);
    app.net_client_list = XInternAtom(app.display, "_NET_CLIENT_LIST", False);
    app.net_wm_name = XInternAtom(app.display, "_NET_WM_NAME", False);
    app.utf8_string = XInternAtom(app.display, "UTF8_STRING", False);
    app.net_wm_state = XInternAtom(app.display, "_NET_WM_STATE", False);
    app.net_wm_state_fullscreen = XInternAtom(app.display, "_NET_WM_STATE_FULLSCREEN", False);
    app.net_wm_state_hidden = XInternAtom(app.display, "_NET_WM_STATE_HIDDEN", False);
    app.net_active_window = XInternAtom(app.display, "_NET_ACTIVE_WINDOW", False);
    app.net_moveresize_window = XInternAtom(app.display, "_NET_MOVERESIZE_WINDOW", False);
    app.net_current_desktop = XInternAtom(app.display, "_NET_CURRENT_DESKTOP", False);
    app.net_wm_desktop = XInternAtom(app.display, "_NET_WM_DESKTOP", False);
    app.wm_state = XInternAtom(app.display, "WM_STATE", False);

    XSelectInput(app.display, app.root, SubstructureNotifyMask | PropertyChangeMask);
    setup_hotkeys(&app);
    app.listen_fd = setup_socket(&app);
    if (app.listen_fd < 0) {
        XCloseDisplay(app.display);
        return 1;
    }

    log_msg("backend=%s monitor=%ux%u+%d+%d thin=%ux%u wide=%ux%u",
            app.config.backend == BACKEND_FRAME_DIRECT ? "frame-direct" : "ewmh",
            app.config.monitor_w,
            app.config.monitor_h,
            app.config.monitor_x,
            app.config.monitor_y,
            app.config.thin_w,
            app.config.thin_h,
            app.config.wide_w,
            app.config.wide_h);

    find_target_window(&app);

    int xfd = ConnectionNumber(app.display);
    time_t last_poll = 0;
    while (running) {
        while (XPending(app.display)) {
            XEvent event;
            XNextEvent(app.display, &event);
            handle_x_event(&app, &event);
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(xfd, &readfds);
        FD_SET(app.listen_fd, &readfds);
        int maxfd = xfd > app.listen_fd ? xfd : app.listen_fd;

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        int rc = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_msg("select failed: %s", strerror(errno));
            break;
        }

        if (rc > 0) {
            if (FD_ISSET(app.listen_fd, &readfds)) {
                handle_client(&app);
            }
            if (FD_ISSET(xfd, &readfds)) {
                while (XPending(app.display)) {
                    XEvent event;
                    XNextEvent(app.display, &event);
                    handle_x_event(&app, &event);
                }
            }
        }

        time_t now = time(NULL);
        if (app.target == None && now != last_poll) {
            last_poll = now;
            find_target_window(&app);
        }
    }

    if (app.listen_fd >= 0) {
        close(app.listen_fd);
        unlink(app.config.socket_path);
    }
    XCloseDisplay(app.display);
    log_msg("stopped");
    return 0;
}
