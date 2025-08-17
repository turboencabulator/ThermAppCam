# SPDX-FileCopyrightText: 2019-2025 Kyle Guinn <elyk03@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

CC = gcc
CFLAGS = -g -O2 -Wall $(shell pkg-config --cflags libusb-1.0)
LDFLAGS = $(shell pkg-config --libs libusb-1.0)

prefix = /usr/local
exec_prefix = $(prefix)
bindir = $(exec_prefix)/bin

thermapp: main.o cal.o usb.o
	$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@
main.o: main.c thermapp.h
cal.o: cal.c thermapp.h
usb.o: usb.c thermapp.h

.PHONY: install
install: thermapp
	install -D thermapp $(DESTDIR)$(bindir)/thermapp

.PHONY: clean
clean:
	rm -f thermapp
	rm -f main.o cal.o usb.o
