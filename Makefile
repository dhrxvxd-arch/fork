PREFIX ?= /usr/local
CC ?= cc
STRIP ?= strip

CFLAGS ?= -std=c23 \
	-Wall -Wextra -Wpedantic \
	-Wno-deprecated-declarations \
	-D_DEFAULT_SOURCE \
	-D_BSD_SOURCE \
	-D_XOPEN_SOURCE=700L \
	-DVERSION=\"0.1.0\" \
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

all: fork

fork: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install: fork
	mkdir -p $(PREFIX)/bin
	cp -f fork $(PREFIX)/bin/fork
	$(STRIP) $(PREFIX)/bin/fork
	chmod 755 $(PREFIX)/bin/fork

uninstall:
	rm -f $(PREFIX)/bin/fork

clean:
	rm -f fork $(OBJ) $(OBJ:.o=.d)

run:
	startx ./xinitrc --

-include $(OBJ:.o=.d)

.PHONY: all clean install uninstall run