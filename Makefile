# wclipmenu — wmenu-based clipboard picker over kaprica (Wayland)
# Zero-footprint, on-demand binary. Depends on libc + external wmenu + kaprica.

VERSION = 0.1

CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -O2 -g
LDFLAGS =

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin

SRC = src/wclipmenu.c
BIN = bin/wclipmenu

all: $(BIN)

$(BIN): $(SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -rf bin

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/wclipmenu

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/wclipmenu

.PHONY: all clean install uninstall
