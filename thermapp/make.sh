#!/bin/bash
gcc main.c thermapp.c `pkg-config --libs --cflags libusb-1.0` -lpthread -o thermapp -Wall
