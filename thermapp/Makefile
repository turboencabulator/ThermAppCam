CC = gcc
CFLAGS = -Wall $(shell pkg-config --cflags libusb-1.0)
LDFLAGS = $(shell pkg-config --libs libusb-1.0) -lpthread

thermapp: main.o thermapp.o
main.o: main.c thermapp.h
thermapp.o: thermapp.c thermapp.h

.PHONY: clean
clean:
	rm -f thermapp
	rm -f main.o thermapp.o
