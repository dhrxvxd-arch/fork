PREFIX ?= /usr/local
CC ?= cc
STRIP ?= strip
VERSION ?= 0.1.0

CFLAGS ?= -std=c23 \
	-Wall -Wextra -Wpedantic \
	-Wno-deprecated-declarations \
	-D_DEFAULT_SOURCE \
	-D_BSD_SOURCE \
	-D_XOPEN_SOURCE=700L \
	-DVERSION=\"$(VERSION)\" \
	-O2 -march=native \
	-flto \
	-ffunction-sections \
	-fdata-sections \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-MMD -MP

LDFLAGS ?= \
	-lxcb \
	-lxcb-keysyms \
	-flto \
	-Wl,--gc-sections \
	-Wl,-O1

SRC = fork.c
OBJ = $(SRC:.c=.o)

DESKTOP = fork.desktop
DESKTOPDIR = $(PREFIX)/share/xsessions

DIST = fork-$(VERSION).tar.xz

all: fork

fork: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(DESKTOP):
	printf '%s\n' \
		'[Desktop Entry]' \
		'Name=Fork' \
		'Comment=Minimal XCB window manager' \
		'Exec=$(PREFIX)/bin/fork' \
		'Type=Application' \
		'DesktopNames=Fork' > $(DESKTOP)

dist: clean
	tar -cJf $(DIST) \
		Makefile \
		fork.c \
		$(DESKTOP)

install: fork $(DESKTOP)
	mkdir -p $(PREFIX)/bin
	mkdir -p $(DESKTOPDIR)

	cp -f fork $(PREFIX)/bin/fork
	cp -f $(DESKTOP) $(DESKTOPDIR)/fork.desktop

	$(STRIP) $(PREFIX)/bin/fork

	chmod 755 $(PREFIX)/bin/fork
	chmod 644 $(DESKTOPDIR)/fork.desktop

	tar -cJf $(DIST) \
		Makefile \
		fork.c \
		$(DESKTOP)

uninstall:
	rm -f $(PREFIX)/bin/fork
	rm -f $(DESKTOPDIR)/fork.desktop

clean:
	rm -f fork $(OBJ) $(OBJ:.o=.d) $(DIST) $(DESKTOP)

run:
	xinit ./fork

-include $(OBJ:.o=.d)

.PHONY: all clean install uninstall run dist
