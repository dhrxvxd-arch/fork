#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#define VERSION "0.1.0"
#define EVENTTYPE(e) ((e)->response_type & ~0x80)
#define ModMask XCB_MOD_MASK_4

typedef struct client {
  xcb_window_t win;
  struct client *next;
} client;

typedef struct atoms {
  xcb_atom_t wm_protocols;
  xcb_atom_t wm_delete;
  xcb_atom_t net_active_window;
  xcb_atom_t net_supported;
} atoms;

typedef struct global {
  int screen_no;
  atoms atoms;
	client *sel;
  client *clients;
  xcb_connection_t *conn;
  xcb_screen_t *screen;
} global;

global *glob;

typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} arg;

static void (*handler[256])(xcb_generic_event_t *);

_Noreturn void die(const char *fmt, ...);
void *ecalloc(size_t nmemb, size_t size);
void spawn(const arg *args);
void setup(void);
void scan(void);
void sigchld(int unused);
void sigterm(int unused);
void startupscan(void);
xcb_atom_t getatom(const char *restrict name);
client *getclient(xcb_window_t win);
void manage(xcb_window_t win);
void unmanage(xcb_window_t win);
void focus(client *c);
void maprequest(xcb_map_request_event_t *e);
void configurerequest(xcb_configure_request_event_t *e);
void destroynotify(xcb_destroy_notify_event_t *e);
void unmapnotify(xcb_unmap_notify_event_t *e);
void enternotify(xcb_enter_notify_event_t *e);
void run(void);
void cleanup(void);
int main(int argc, char **argv);

_Noreturn void die(const char *fmt, ...) {
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

void *ecalloc(size_t nmemb, size_t size) {
  void *p;

  if (!(p = calloc(nmemb, size)))
    die("calloc:");
  return p;
}

void spawn(const arg *args) {
  struct sigaction sa;

  if (!fork()) {
		if (glob->conn)
			close(xcb_get_file_descriptor(glob->conn));
    setsid();

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &sa, NULL);

    execvp(*((char **)args->v), (char **)args->v);
    die("fork: execvp '%s' failed:", *((char **)args->v));
  }
}

void sigchld(int unused) {
  (void)unused;

  while (waitpid(-1, NULL, WNOHANG) > 0)
    ;
}

void sigterm(int unused) {
  (void)unused;
  cleanup();
  exit(0);
}

xcb_atom_t getatom(const char *restrict name) {
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

client *getclient(xcb_window_t win) {
  client *c;

  for (c = glob->clients; c; c = c->next)
    if (c->win == win)
      return c;

  return NULL;
}

void manage(xcb_window_t win) {
  client *c;
  xcb_get_window_attributes_cookie_t cookie;
  xcb_get_window_attributes_reply_t *attrs;

  if (getclient(win))
    return;

  cookie = xcb_get_window_attributes(glob->conn, win);
  attrs = xcb_get_window_attributes_reply(glob->conn, cookie, NULL);

  if (!attrs)
    return;

  if (attrs->override_redirect) {
    free(attrs);
    return;
  }

  free(attrs);

  c = ecalloc(1, sizeof *c);
  c->win = win;
  c->next = glob->clients;
  glob->clients = c;
}

void unmanage(xcb_window_t win) {
  client **c;

  for (c = &glob->clients; *c; c = &(*c)->next) {
    if ((*c)->win == win) {
      client *tmp = *c;
      *c = (*c)->next;
      free(tmp);
      return;
    }
  }
}

void setup(void) {
  struct sigaction sa;

  glob = ecalloc(1, sizeof *glob);
  glob->conn = xcb_connect(NULL, &glob->screen_no);

  if (!glob->conn || xcb_connection_has_error(glob->conn))
    die("Failed to connect to X server");

  glob->screen = xcb_setup_roots_iterator(xcb_get_setup(glob->conn)).data;

  glob->atoms.wm_protocols = getatom("WM_PROTOCOLS");
  glob->atoms.wm_delete = getatom("WM_DELETE_WINDOW");
  glob->atoms.net_active_window = getatom("_NET_ACTIVE_WINDOW");
  glob->atoms.net_supported = getatom("_NET_SUPPORTED");

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

  xcb_flush(glob->conn);
}

void scan(void) {
  uint32_t values[] = {
      XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE |
      XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION |
      XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
      XCB_EVENT_MASK_STRUCTURE_NOTIFY};

  xcb_void_cookie_t cookie;
  xcb_generic_error_t *err;

  cookie = xcb_change_window_attributes_checked(glob->conn, glob->screen->root,
                                                XCB_CW_EVENT_MASK, values);

  xcb_flush(glob->conn);

  if ((err = xcb_request_check(glob->conn, cookie)))
    die("another window manager is already running");
}

void startupscan(void) {
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

void focus(client *c) {
  if (!c)
    return;

  glob->sel = c;

  xcb_set_input_focus(glob->conn, XCB_INPUT_FOCUS_POINTER_ROOT, c->win,
                      XCB_CURRENT_TIME);

  xcb_flush(glob->conn);
}

void maprequest(xcb_map_request_event_t *e) {
	client *c; 
  manage(e->window);
  xcb_map_window(glob->conn, e->window);
	if ((c = getclient(e->window)))
		focus(c);
}

void configurerequest(xcb_configure_request_event_t *e) {
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
}

void destroynotify(xcb_destroy_notify_event_t *e) { 
	unmanage(e->window); 
}

void unmapnotify(xcb_unmap_notify_event_t *e) {
	unmanage(e->window);
}

void enternotify(xcb_enter_notify_event_t *e) {
  client *c;

  if ((c = getclient(e->event)))
    focus(c);
}

void run(void) {
  xcb_generic_event_t *ev;
  uint8_t type;

  while ((ev = xcb_wait_for_event(glob->conn))) {
    type = EVENTTYPE(ev);

    if (handler[type])
      handler[type](ev);

    free(ev);
    xcb_flush(glob->conn);
  }
}

void cleanup(void) {
  client *c;
  client *next;

  for (c = glob->clients; c; c = next) {
    next = c->next;
    free(c);
  }

  xcb_disconnect(glob->conn);
  free(glob);
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "-v"))
    die("fork-" VERSION);
  else if (argc != 1)
    die("usage: fork [-v]");

  if (!setlocale(LC_CTYPE, ""))
    die("warning: no locale support");

  setup();

#ifdef __OpenBSD__
  if (pledge("stdio rpath proc exec", NULL) == -1)
    die("pledge");
#endif

  scan();
  startupscan();
  run();
  cleanup();

  return 0;
}