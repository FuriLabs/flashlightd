CC = gcc
CFLAGS = $(shell pkg-config --cflags gio-2.0 gstreamer-1.0)
LDFLAGS = $(shell pkg-config --libs gio-2.0 gstreamer-1.0)

SOURCES = src/flashlightd.c
TARGET = flashlightd

PREFIX ?= /usr

all: $(TARGET)

$(TARGET):
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/libexec/$(TARGET)
	install -D -m 644 flashlightd.service $(DESTDIR)$(PREFIX)/lib/systemd/user/flashlightd.service

clean:
	rm -f $(TARGET)

.PHONY: all install clean
