VERSION := 1.0.0
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -DT2TOP_VERSION=\"$(VERSION)\"
LDLIBS   = -lm
PREFIX  ?= /usr/local

SRC := src/util.c src/tui.c src/sensors.c src/main.c
OBJ := $(SRC:.c=.o)

t2top: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/util.h src/tui.h src/sensors.h
	$(CC) $(CFLAGS) -c -o $@ $<

debug: clean
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -D_DEFAULT_SOURCE \
	  -DT2TOP_VERSION=\"$(VERSION)-debug\" -Wall -Wextra \
	  -o t2top-debug $(SRC) $(LDLIBS)

install: t2top
	install -Dm755 t2top $(DESTDIR)$(PREFIX)/bin/t2top
	install -Dm644 t2top.1 $(DESTDIR)$(PREFIX)/share/man/man1/t2top.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/t2top $(DESTDIR)$(PREFIX)/share/man/man1/t2top.1

clean:
	rm -f $(OBJ) t2top t2top-debug

.PHONY: debug install uninstall clean
