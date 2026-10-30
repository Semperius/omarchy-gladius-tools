CC ?= gcc
CFLAGS ?= -O2 -Wall
PKG_CONFIG ?= pkg-config
DEPS = gtk4 gtk4-layer-shell-0 cairo alsa
CFLAGS += $(shell $(PKG_CONFIG) --cflags $(DEPS))
LIBS += $(shell $(PKG_CONFIG) --libs $(DEPS)) -lm -lpthread

TARGET = gladius-smoke-trail
PREFIX ?= $(HOME)/.local/bin

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

install: $(TARGET)
	install -d $(PREFIX)
	install -m 755 $(TARGET) $(PREFIX)/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install clean
