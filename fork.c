#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#define VERSION "0.1.0"

_Noreturn void die(const char *fmt, ...);
void *ecalloc(size_t nmemb, size_t size);
void setup(void);
void scan(void);
void maprequest(xcb_map_request_event_t *e);
void configurerequest(xcb_configure_request_event_t *e);
void run(void);
int main(int argc, char **argv);

typedef struct global {
  xcb_connection_t *conn;
  xcb_screen_t *screen;
} global;

global *glob;

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

void setup(void) {
  glob = ecalloc(1, sizeof *glob);

  glob->conn = xcb_connect(NULL, NULL);

  if (!glob->conn || xcb_connection_has_error(glob->conn))
    die("Failed to connect to X server");

  glob->screen = xcb_setup_roots_iterator(xcb_get_setup(glob->conn)).data;

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

void run(void) {
  xcb_generic_event_t *ev;

  while ((ev = xcb_wait_for_event(glob->conn))) {
		switch(ev->response_type & ~0x80) {
			case XCB_MAP_REQUEST: {
				maprequest((xcb_map_request_event_t *)ev);
			} break;
		}
		free(ev);
	}
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "-v"))
    die("fork-" VERSION);
  else if (argc != 1)
    die("usage: fork [-v]");

  setup();
  scan();
  run();

  xcb_disconnect(glob->conn);
  free(glob);

  return 0;
}
