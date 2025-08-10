// SPDX-FileCopyrightText: 2015 Alexander G <pidbip@gmail.com>
// SPDX-FileCopyrightText: 2019-2025 Kyle Guinn <elyk03@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THERMAPP_H
#define THERMAPP_H

#include <libusb.h>
#include <pthread.h>
#include <stdint.h>

#define VENDOR  0x1772
#define PRODUCT 0x0002

#define HEADER_SIZE   64
#define FRAME_WIDTH  384
#define FRAME_HEIGHT 288
#define FRAME_PIXELS (FRAME_WIDTH * FRAME_HEIGHT)
#define FRAME_SIZE   (HEADER_SIZE + 2*FRAME_PIXELS)

// Device apparently only works with 512-byte chunks of data.
// Note the frame is padded to a multiple of 512 bytes.
#define CHUNK_SIZE   512
#define FRAME_PADDED_SIZE ((FRAME_SIZE+CHUNK_SIZE-1) & ~(CHUNK_SIZE-1))

// USB bulk transfers should ideally be FRAME_PADDED_SIZE but we support smaller.
#define TRANSFER_SIZE 8192
#if (TRANSFER_SIZE % CHUNK_SIZE) || (TRANSFER_SIZE < CHUNK_SIZE)
#error TRANSFER_SIZE must be a multiple of CHUNK_SIZE
#endif
#if (TRANSFER_SIZE > FRAME_PADDED_SIZE)
#error TRANSFER_SIZE must not be larger than FRAME_PADDED_SIZE
#endif

#define CAL_FILES 12

// AD5628 DAC in Therm App is for generating control voltage
// VREF = 2.5 volts 11 Bit
struct thermapp_cfg {
	uint16_t preamble[4];
	uint16_t modes;// 0xXXXM  Modes set last nibble
	uint16_t serial_num_lo;
	uint16_t serial_num_hi;
	uint16_t hardware_ver;
	uint16_t firmware_ver;
	uint16_t fpa_h;
	uint16_t fpa_w;
	uint16_t data_0b;
	uint16_t data_0c;
	uint16_t data_0d;
	uint16_t data_0e;
	int16_t temperature;
	uint16_t VoutA; //DCoffset;// AD5628 VoutA, Range: 0V - 2.45V, max 2048
	uint16_t data_11;
	uint16_t VoutC;//gain;// AD5628 VoutC, Range: 0V - 3.59V, max 2984 ??????
	uint16_t VoutD;// AD5628 VoutD, Range: 0V - 2.895V, max 2394 ??????
	uint16_t VoutE;// AD5628 VoutE, Range: 0V - 3.63V, max 2997, FPA VBUS
	uint16_t data_15;
	uint16_t data_16;
	uint16_t data_17;
	uint16_t data_18;
	uint16_t data_offset; // or header_size?
	uint16_t frame_count;
	uint16_t data_1b;
	uint16_t data_1c;
	uint16_t data_1d;
	uint16_t data_1e;
	uint16_t data_1f;
};

union thermapp_frame {
	struct thermapp_cfg header;
	unsigned char bytes[FRAME_PADDED_SIZE];
};

struct thermapp_usb_dev {
	libusb_context *ctx;
	libusb_device_handle *usb;
	struct libusb_transfer *transfer_in;
	struct libusb_transfer *transfer_out;

	int read_async_started;
	int read_async_completed;
	pthread_t pthread_read_async;
	pthread_mutex_t mutex_frame_swap;
	pthread_cond_t cond_frame_ready;

	unsigned char *cfg;
	unsigned char *frame_in;
	unsigned char *frame_done;
};

struct thermapp_cal {
	uint32_t serial_num;

	char *path_buf;
	char *leaf_ptr;
	size_t leaf_len;

	unsigned char *raw_buf[CAL_FILES];
	size_t raw_len[CAL_FILES];
};


struct thermapp_usb_dev *thermapp_usb_open(void);
int thermapp_usb_thread_create(struct thermapp_usb_dev *);
size_t thermapp_usb_frame_read(struct thermapp_usb_dev *, void *, size_t);
void thermapp_usb_close(struct thermapp_usb_dev *);

struct thermapp_cal *thermapp_cal_open(const char *, uint32_t);
void thermapp_cal_close(struct thermapp_cal *);

#endif /* THERMAPP_H */
