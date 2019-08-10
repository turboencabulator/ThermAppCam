CC = gcc
CFLAGS = -Wall
LDFLAGS = $(shell pkg-config --libs --cflags libusb-1.0) -lpthread

thermapp: main.c thermapp.c

.PHONY: clean
clean:
	rm thermapp
