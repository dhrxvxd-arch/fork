#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xproto.h>
#include <X11/keysym.h>

#define VERSION "0.1.0"
#define EVENTTYPE(e) ((e)->response_type & ~0x80)
#define LENGTH(X) (sizeof(X) / sizeof((X)[0]))
#define CLEANMASK(mask)                                                        \
  (mask & ~(numlockmask | XCB_MOD_MASK_LOCK) &                                 \
   (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 |               \
    XCB_MOD_MASK_2 | XCB_MOD_MASK_3 | XCB_MOD_MASK_4 | XCB_MOD_MASK_5))

#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_FMT(a, b) __attribute__((format(printf, a, b)))
#else
#define PRINTF_FMT(a, b)
#endif

typedef struct client {
  xcb_window_t win;
  struct client *next;
  int fullscreen;
} client;

typedef struct atoms {
  xcb_atom_t wm_protocols;
  xcb_atom_t wm_delete;
  xcb_atom_t net_active_window;
  xcb_atom_t net_supported;
  xcb_atom_t net_wm_state;
  xcb_atom_t net_wm_state_fullscreen;
  xcb_atom_t net_wm_state_maximized_horz;
  xcb_atom_t net_wm_state_maximized_vert;
  xcb_atom_t net_client_list;
  xcb_atom_t net_current_desktop;
  xcb_atom_t net_number_of_desktops;
} atoms;

typedef struct global {
  int screen_no;
  atoms atoms;
  client *sel;
  client *clients;
  xcb_connection_t *conn;
  xcb_screen_t *screen;
  xcb_key_symbols_t *syms;
} global;

typedef union {
  int i;
  unsigned int ui;
  float f;
  const void *v;
} arg;

typedef struct key {
  uint16_t mod;
  xcb_keysym_t keysym;
  void (*func)(const arg *);
  const arg arg;
} key;

static global *glob;
static volatile sig_atomic_t running = 1;
static unsigned int numlockmask;
static void (*handler[256])(xcb_generic_event_t *);

static _Noreturn void die(const char *fmt, ...) PRINTF_FMT(1, 2);
static void *ecalloc(size_t nmemb, size_t size);
static void spawn(const arg *args);
static void keypress(xcb_key_press_event_t *e);
static void grabkeys(void);
static void setup(void);
static void scan(void);
static void sigchld(int unused);
static void sigterm(int unused);
static void startupscan(void);
static void updatenumlockmask(void);
static xcb_atom_t getatom(const char *restrict name);
static client *getclient(xcb_window_t win);
static void manage(xcb_window_t win);
static void unmanage(xcb_window_t win);
static void tile(void);
static void monocle(void);
static void fullscreen(client *c);
static void setfullscreen(xcb_window_t win, int enable);
static void togglefullscreen(const arg *a);
static void focus(client *c);
static void arrange(const arg *args);
static void maprequest(xcb_map_request_event_t *e);
static void configurerequest(xcb_configure_request_event_t *e);
static void destroynotify(xcb_destroy_notify_event_t *e);
static void unmapnotify(xcb_unmap_notify_event_t *e);
static void enternotify(xcb_enter_notify_event_t *e);
static void run(void);
static void cleanup(void);

static const char *termcmd[] = {"alacritty", NULL};

#define ModMask XCB_MOD_MASK_4

enum { LAYOUT_TILE, LAYOUT_MONOCLE };

static const key keys[] = {
    {ModMask, XK_Return, spawn, {.v = termcmd}},
    {ModMask, XK_t, arrange, {.i = LAYOUT_TILE}},
    {ModMask, XK_m, arrange, {.i = LAYOUT_MONOCLE}},
    {ModMask, XK_space, togglefullscreen, {0}},
};

static _Noreturn void die(const char *fmt, ...) {
  va_list ap;
  int saved_errno = errno;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  if (*fmt && fmt[strlen(fmt) - 1] == ':')
    fprintf(stderr, " %s", strerror(saved_errno));

  fputc('\n', stderr);
  exit(1);
}

static void *ecalloc(size_t nmemb, size_t size) {
  void *p;

  if (!(p = calloc(nmemb, size)))
    die("calloc:");

  return p;
}

static void spawn(const arg *args) {
  struct sigaction sa;
  pid_t pid;

  if ((pid = fork()) == 0) {
    if (glob->conn)
      close(xcb_get_file_descriptor(glob->conn));

    setsid();

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &sa, NULL);

    execvp(*((char **)args->v), (char **)args->v);
    die("execvp '%s' failed:", *((char **)args->v));
  } else if (pid < 0)
    die("fork:");
}

static void keypress(xcb_key_press_event_t *e) {
  xcb_keysym_t sym;
  unsigned int i;

  sym = xcb_key_symbols_get_keysym(glob->syms, e->detail, 0);

  for (i = 0; i < LENGTH(keys); i++)
    if (sym == keys[i].keysym &&
        CLEANMASK(e->state) == CLEANMASK(keys[i].mod) && keys[i].func)
      keys[i].func(&(keys[i].arg));
}

static void grabkeys(void) {
  xcb_keycode_t *codes;
  unsigned int i;
  int j;

  xcb_ungrab_key(glob->conn, XCB_GRAB_ANY, glob->screen->root,
                 XCB_MOD_MASK_ANY);

  for (i = 0; i < LENGTH(keys); i++) {
    if (!(codes = xcb_key_symbols_get_keycode(glob->syms, keys[i].keysym)))
      continue;

    for (j = 0; codes[j] != XCB_NO_SYMBOL; j++)
      xcb_grab_key(glob->conn, 1, glob->screen->root, keys[i].mod | numlockmask,
                   codes[j], XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

    free(codes);
  }
}

static void sigchld(int unused) {
  (void)unused;

  while (waitpid(-1, NULL, WNOHANG) > 0)
    ;
}

static void sigterm(int unused) {
  (void)unused;
  running = 0;
}

static xcb_atom_t getatom(const char *restrict name) {
  xcb_intern_atom_cookie_t cookie;
  xcb_intern_atom_reply_t *reply;
  xcb_atom_t atom;

  cookie = xcb_intern_atom(glob->conn, 0, strlen(name), name);
  reply = xcb_intern_atom_reply(glob->conn, cookie, NULL);

  if (!reply)
    die("xcb_intern_atom_reply");

  atom = reply->atom;
  free(reply);

  return atom;
}

static client *getclient(xcb_window_t win) {
  client *c;

  for (c = glob->clients; c; c = c->next)
    if (c->win == win)
      return c;

  return NULL;
}

static void manage(xcb_window_t win) {
  client *c;
  xcb_get_window_attributes_cookie_t cookie;
  xcb_get_window_attributes_reply_t *attrs;

  if (getclient(win))
    return;

  cookie = xcb_get_window_attributes(glob->conn, win);
  attrs = xcb_get_window_attributes_reply(glob->conn, cookie, NULL);

  if (!attrs)
    return;

  if (attrs->override_redirect || attrs->map_state != XCB_MAP_STATE_VIEWABLE) {
    free(attrs);
    return;
  }

  free(attrs);

  c = ecalloc(1, sizeof *c);
  c->win = win;
  c->next = glob->clients;
  glob->clients = c;
}

static void unmanage(xcb_window_t win) {
  client **c;

  for (c = &glob->clients; *c; c = &(*c)->next) {
    if ((*c)->win == win) {
      client *tmp = *c;
      *c = (*c)->next;

      if (glob->sel == tmp)
        glob->sel = NULL;

      free(tmp);
      return;
    }
  }
}

static void updatenumlockmask(void) {
  xcb_get_modifier_mapping_cookie_t cookie;
  xcb_get_modifier_mapping_reply_t *reply;
  xcb_keycode_t *codes;
  xcb_keycode_t *numlockcodes;
  int keycodespermod;
  int mod;
  int key;

  numlockmask = 0;

  cookie = xcb_get_modifier_mapping(glob->conn);
  reply = xcb_get_modifier_mapping_reply(glob->conn, cookie, NULL);

  if (!reply)
    return;

  keycodespermod = reply->keycodes_per_modifier;
  codes = xcb_get_modifier_mapping_keycodes(reply);

  numlockcodes = xcb_key_symbols_get_keycode(glob->syms, XK_Num_Lock);

  if (!numlockcodes) {
    free(reply);
    return;
  }

  for (mod = 0; mod < 8; mod++) {
    for (key = 0; key < keycodespermod; key++) {
      xcb_keycode_t code = codes[mod * keycodespermod + key];

      for (int i = 0; numlockcodes[i] != XCB_NO_SYMBOL; i++) {
        if (code == numlockcodes[i]) {
          numlockmask = (1 << mod);
          goto done;
        }
      }
    }
  }

done:
  free(numlockcodes);
  free(reply);
}

static void setup(void) {
  struct sigaction sa;

  glob = ecalloc(1, sizeof *glob);
  glob->conn = xcb_connect(NULL, &glob->screen_no);

  if (!glob->conn || xcb_connection_has_error(glob->conn))
    die("Failed to connect to X server");

  xcb_screen_iterator_t iter;

  iter = xcb_setup_roots_iterator(xcb_get_setup(glob->conn));

  for (int i = 0; i < glob->screen_no; i++)
    xcb_screen_next(&iter);

  glob->screen = iter.data;

  glob->atoms.wm_protocols = getatom("WM_PROTOCOLS");
  glob->atoms.wm_delete = getatom("WM_DELETE_WINDOW");
  glob->atoms.net_active_window = getatom("_NET_ACTIVE_WINDOW");
  glob->atoms.net_supported = getatom("_NET_SUPPORTED");
  glob->atoms.net_wm_state = getatom("_NET_WM_STATE");
  glob->atoms.net_wm_state_fullscreen = getatom("_NET_WM_STATE_FULLSCREEN");
  glob->atoms.net_wm_state_maximized_horz = getatom("_NET_WM_STATE_MAXIMIZED_HORZ");
  glob->atoms.net_wm_state_maximized_vert = getatom("_NET_WM_STATE_MAXIMIZED_VERT");
  glob->atoms.net_client_list = getatom("_NET_CLIENT_LIST");
  glob->atoms.net_current_desktop = getatom("_NET_CURRENT_DESKTOP");
  glob->atoms.net_number_of_desktops = getatom("_NET_NUMBER_OF_DESKTOPS");

  glob->syms = xcb_key_symbols_alloc(glob->conn);

  updatenumlockmask();

  memset(&sa, 0, sizeof sa);
  sa.sa_handler = sigchld;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, NULL);

  memset(&sa, 0, sizeof sa);
  sa.sa_handler = sigterm;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  handler[XCB_MAP_REQUEST] = (void (*)(xcb_generic_event_t *))maprequest;

  handler[XCB_CONFIGURE_REQUEST] =
      (void (*)(xcb_generic_event_t *))configurerequest;

  handler[XCB_DESTROY_NOTIFY] = (void (*)(xcb_generic_event_t *))destroynotify;

  handler[XCB_UNMAP_NOTIFY] = (void (*)(xcb_generic_event_t *))unmapnotify;

  handler[XCB_ENTER_NOTIFY] = (void (*)(xcb_generic_event_t *))enternotify;

  handler[XCB_KEY_PRESS] = (void (*)(xcb_generic_event_t *))keypress;

  xcb_flush(glob->conn);
}

static void scan(void) {
  uint32_t values[] = {
      XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE |
      XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION |
      XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
      XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS};

  xcb_void_cookie_t cookie;
  xcb_generic_error_t *err;

  cookie = xcb_change_window_attributes_checked(glob->conn, glob->screen->root,
                                                XCB_CW_EVENT_MASK, values);

  xcb_flush(glob->conn);

  if ((err = xcb_request_check(glob->conn, cookie))) {
    free(err);
    die("another window manager is already running");
  }
}

static void startupscan(void) {
  xcb_query_tree_cookie_t cookie;
  xcb_query_tree_reply_t *reply;
  xcb_window_t *wins;
  int len;
  int i;

  cookie = xcb_query_tree(glob->conn, glob->screen->root);

  reply = xcb_query_tree_reply(glob->conn, cookie, NULL);

  if (!reply)
    return;

  wins = xcb_query_tree_children(reply);
  len = xcb_query_tree_children_length(reply);

  for (i = 0; i < len; i++)
    manage(wins[i]);

  free(reply);
}

static void fullscreen(client *c) {
  uint32_t v[4];

  v[0] = 0;
  v[1] = 0;
  v[2] = glob->screen->width_in_pixels;
  v[3] = glob->screen->height_in_pixels;

  xcb_configure_window(
    glob->conn,
    c->win,
    XCB_CONFIG_WINDOW_X |
    XCB_CONFIG_WINDOW_Y |
    XCB_CONFIG_WINDOW_WIDTH |
    XCB_CONFIG_WINDOW_HEIGHT,
    v
  );
}

static void setfullscreen(xcb_window_t win, int enable) {
  xcb_client_message_event_t ev;

  memset(&ev, 0, sizeof(ev));

  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.window = win;
  ev.type = glob->atoms.net_wm_state;
  ev.format = 32;

  ev.data.data32[0] = enable;
  ev.data.data32[1] = glob->atoms.net_wm_state_fullscreen;
  ev.data.data32[2] = XCB_NONE;
  ev.data.data32[3] = 0;
  ev.data.data32[4] = 0;

  xcb_send_event(glob->conn, 0, glob->screen->root,
                 XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                     XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                 (char *)&ev);

  xcb_flush(glob->conn);
}

static void togglefullscreen(const arg *a) {
  (void)a;
  client *c;

  if (!(c = glob->sel))
    return;

  c->fullscreen = !c->fullscreen;

  setfullscreen(c->win, c->fullscreen);

  focus(c);
}

static void tile(void) {
  client *c;
  int n = 0;

  for (c = glob->clients; c; c = c->next)
    n++;

  if (!n)
    return;

  int sw = glob->screen->width_in_pixels;
  int sh = glob->screen->height_in_pixels;

  int masterw = (n == 1) ? sw : sw * 0.6;
  int stackw = sw - masterw;

  int i = 0;
  int y = 0;

  for (c = glob->clients; c; c = c->next, i++) {
    uint32_t v[4];

    if (i == 0) {
      v[0] = 0;
      v[1] = 0;
      v[2] = masterw;
      v[3] = sh;
    } else {
      int h = sh / (n - 1);

      v[0] = masterw;
      v[1] = y;
      v[2] = stackw;
      v[3] = h;

      y += h;
    }

    xcb_configure_window(
      glob->conn,
      c->win,
      XCB_CONFIG_WINDOW_X |
      XCB_CONFIG_WINDOW_Y |
      XCB_CONFIG_WINDOW_WIDTH |
      XCB_CONFIG_WINDOW_HEIGHT,
      v
    );
  }

  xcb_flush(glob->conn);
}

static void monocle(void) {
  client *c;

  for (c = glob->clients; c; c = c->next)
    fullscreen(c);

  xcb_flush(glob->conn);
}

static void focus(client *c) {
  if (!c || glob->sel == c)
    return;

  glob->sel = c;

  xcb_set_input_focus(glob->conn, XCB_INPUT_FOCUS_POINTER_ROOT, c->win,
                      XCB_CURRENT_TIME);

  uint32_t v = XCB_STACK_MODE_ABOVE;

  xcb_configure_window(glob->conn, c->win, XCB_CONFIG_WINDOW_STACK_MODE, &v);

  xcb_flush(glob->conn);
}

static void arrange(const arg *args) {
  if (!args)
    return;

  switch (args->i) {
  case 0:
    tile();
    break;
  case 1:
    monocle();
    break;
  }
}

static void maprequest(xcb_map_request_event_t *e) {
  client *c;

  manage(e->window);

  xcb_map_window(glob->conn, e->window);

  if ((c = getclient(e->window)))
    focus(c);

  xcb_flush(glob->conn);
}

static void configurerequest(xcb_configure_request_event_t *e) {
  uint32_t values[7];
  uint32_t i = 0;

  if (e->value_mask & XCB_CONFIG_WINDOW_X)
    values[i++] = e->x;

  if (e->value_mask & XCB_CONFIG_WINDOW_Y)
    values[i++] = e->y;

  if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)
    values[i++] = e->width;

  if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
    values[i++] = e->height;

  if (e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
    values[i++] = e->border_width;

  if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING)
    values[i++] = e->sibling;

  if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
    values[i++] = e->stack_mode;

  xcb_configure_window(glob->conn, e->window, e->value_mask, values);

  xcb_flush(glob->conn);
}

static void destroynotify(xcb_destroy_notify_event_t *e) {
  unmanage(e->window);
}

static void unmapnotify(xcb_unmap_notify_event_t *e) { unmanage(e->window); }

static void enternotify(xcb_enter_notify_event_t *e) {
  client *c;

  if ((c = getclient(e->event)))
    focus(c);
}

static void run(void) {
  xcb_generic_event_t *ev;
  uint8_t type;

  while (running && (ev = xcb_wait_for_event(glob->conn))) {
    type = EVENTTYPE(ev);

    if (handler[type])
      handler[type](ev);

    free(ev);
  }

  if (xcb_connection_has_error(glob->conn))
    die("X connection lost");
}

static void cleanup(void) {
  client *c;
  client *next;

  for (c = glob->clients; c; c = next) {
    next = c->next;
    free(c);
  }

  xcb_key_symbols_free(glob->syms);
  xcb_disconnect(glob->conn);
  free(glob);
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "-v"))
    die("fork-" VERSION);

  else if (argc != 1)
    die("usage: fork [-v]");

  if (!setlocale(LC_CTYPE, ""))
    fprintf(stderr, "warning: no locale support");

  setup();

#ifdef __OpenBSD__
  if (pledge("stdio rpath proc exec", NULL) == -1)
    die("pledge");
#endif

  scan();
  grabkeys();
  startupscan();
  run();
  cleanup();

  return 0;
}
