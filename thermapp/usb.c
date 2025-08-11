// SPDX-FileCopyrightText: 2015 Alexander G <pidbip@gmail.com>
// SPDX-FileCopyrightText: 2019-2025 Kyle Guinn <elyk03@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermapp.h"

#include <endian.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char preamble[] = {
	0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xd5, 0xa5,
};

// These are host-endian, must be converted to little-endian before transfer
static const struct thermapp_cfg initial_cfg = {
	.preamble[0] = 0xa5a5,
	.preamble[1] = 0xa5a5,
	.preamble[2] = 0xa5a5,
	.preamble[3] = 0xa5d5,
	.modes       = 0x0002, // (control) // test pattern low
	//.serial_num_lo = 0, // (status)
	//.serial_num_hi = 0, // (status)
	//.hardware_ver = 0, // (status)
	//.firmware_ver = 0, // (status)
	.fpa_h       = FRAME_HEIGHT, // (status)
	.fpa_w       = FRAME_WIDTH, // (status)
	.data_0b     = FRAME_HEIGHT, // (control)
	.data_0c     = FRAME_WIDTH, // (control)
	.data_0d     = 0x0019,
	.data_0e     = 0x0000,
	//.temperature = 0, // (status)
	.VoutA       = 0x075c,
	.data_11     = 0x0b85,
	.VoutC       = 0x05f4,
	.VoutD       = 0x0800,
	.VoutE       = 0x0b85,
	.data_15     = 0x0b85,
	.data_16     = 0x0000,
	.data_17     = 0x0570,
	.data_18     = 0x0b85,
	.data_offset = HEADER_SIZE, // (status)
	//.frame_count = 0, // (status)
	.data_1b     = 0x0000,
	.data_1c     = 0x0050,
	.data_1d     = 0x0003,
	.data_1e     = 0x0000,
	.data_1f     = 0x0fff,
};

static void
cancel_async(struct thermapp_usb_dev *dev)
{
	if (dev->transfer_in) {
		int ret = libusb_cancel_transfer(dev->transfer_in);
		if (ret && ret != LIBUSB_ERROR_NOT_FOUND) {
			fprintf(stderr, "%s: %s\n", "libusb_cancel_transfer", libusb_strerror(ret));
		}
	}

	if (dev->transfer_out) {
		int ret = libusb_cancel_transfer(dev->transfer_out);
		if (ret && ret != LIBUSB_ERROR_NOT_FOUND) {
			fprintf(stderr, "%s: %s\n", "libusb_cancel_transfer", libusb_strerror(ret));
		}
	}
}

static void LIBUSB_CALL
transfer_cb_out(struct libusb_transfer *transfer)
{
	struct thermapp_usb_dev *dev = (struct thermapp_usb_dev *)transfer->user_data;

	transfer->buffer = NULL;

	if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		cancel_async(dev);
	}
}

static void LIBUSB_CALL
transfer_cb_in(struct libusb_transfer *transfer)
{
	struct thermapp_usb_dev *dev = (struct thermapp_usb_dev *)transfer->user_data;

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		if (transfer->actual_length % CHUNK_SIZE) {
			fprintf(stderr, "discarding partial transfer of size %u\n", transfer->actual_length);
			transfer->buffer = dev->frame_in;
			transfer->length = TRANSFER_SIZE;
		} else if (transfer->actual_length) {
			unsigned char *buf = dev->frame_in;
			size_t old = (unsigned char *)transfer->buffer - buf;
			size_t len = old + transfer->actual_length;

			if (!old) {
				// Sync to start of frame.
				// Look for preamble at start of chunk.
				while (len >= CHUNK_SIZE) {
					if (memcmp(buf, preamble, sizeof preamble) == 0) {
						break;
					}
					buf += CHUNK_SIZE;
					len -= CHUNK_SIZE;
				}
				memmove(dev->frame_in, buf, len);
			}

			if (len == FRAME_PADDED_SIZE) {
				// Frame complete.
				pthread_mutex_lock(&dev->mutex_frame_swap);
				unsigned char *tmp = dev->frame_done;
				dev->frame_done = dev->frame_in;
				dev->frame_in = tmp;
				pthread_cond_broadcast(&dev->cond_frame_ready);
				pthread_mutex_unlock(&dev->mutex_frame_swap);

				transfer->buffer = dev->frame_in;
				len = 0;
			}

			transfer->buffer = dev->frame_in + len;
			transfer->length = TRANSFER_SIZE;
			if (transfer->length > FRAME_PADDED_SIZE - len) {
				transfer->length = FRAME_PADDED_SIZE - len;
			}
		}

		int ret = libusb_submit_transfer(transfer);
		if (ret) {
			fprintf(stderr, "%s: %s\n", "libusb_submit_transfer", libusb_strerror(ret));
			transfer->buffer = NULL;
		}
	} else {
		transfer->buffer = NULL;

		cancel_async(dev);
	}
}

static void *
read_async(void *ctx)
{
	struct thermapp_usb_dev *dev = (struct thermapp_usb_dev *)ctx;
	int ret;

	libusb_fill_bulk_transfer(dev->transfer_out,
	                          dev->usb,
	                          LIBUSB_ENDPOINT_OUT | 2,
	                          dev->cfg,
	                          HEADER_SIZE,
	                          transfer_cb_out,
	                          dev,
	                          0);
	ret = libusb_submit_transfer(dev->transfer_out);
	if (ret) {
		fprintf(stderr, "%s: %s\n", "libusb_submit_transfer", libusb_strerror(ret));
		dev->transfer_out->buffer = NULL;
	}

	libusb_fill_bulk_transfer(dev->transfer_in,
	                          dev->usb,
	                          LIBUSB_ENDPOINT_IN | 1,
	                          dev->frame_in,
	                          TRANSFER_SIZE,
	                          transfer_cb_in,
	                          (void *)dev,
	                          0);
	ret = libusb_submit_transfer(dev->transfer_in);
	if (ret) {
		fprintf(stderr, "%s: %s\n", "libusb_submit_transfer", libusb_strerror(ret));
		dev->transfer_in->buffer = NULL;
	}

	// Using transfer->buffer as an indication that transfers are pending.
	while (dev->transfer_out->buffer || dev->transfer_in->buffer) {
		ret = libusb_handle_events(dev->ctx);
		if (ret) {
			fprintf(stderr, "%s: %s\n", "libusb_handle_events", libusb_strerror(ret));
			if (ret == LIBUSB_ERROR_INTERRUPTED) /* stray signal */ {
				continue;
			} else {
				break;
			}
		}
	}

	// All transfers completed or cancelled.  Wake all waiters so they can exit.
	pthread_mutex_lock(&dev->mutex_frame_swap);
	dev->read_async_completed = 1;
	pthread_cond_broadcast(&dev->cond_frame_ready);
	pthread_mutex_unlock(&dev->mutex_frame_swap);

	return NULL;
}

struct thermapp_usb_dev *
thermapp_usb_open(void)
{
	int ret;

	struct thermapp_usb_dev *dev = calloc(1, sizeof *dev);
	if (!dev) {
		perror("calloc");
		goto err;
	}

	ret = libusb_init(&dev->ctx);
	if (ret) {
		fprintf(stderr, "%s: %s\n", "libusb_init", libusb_strerror(ret));
		goto err;
	}

	dev->usb = libusb_open_device_with_vid_pid(dev->ctx, VENDOR, PRODUCT);
	if (!dev->usb) {
		ret = LIBUSB_ERROR_NO_DEVICE;
		fprintf(stderr, "%s: %s\n", "libusb_open_device_with_vid_pid", libusb_strerror(ret));
		goto err;
	}

	ret = libusb_set_configuration(dev->usb, 1);
	if (ret) {
		fprintf(stderr, "%s: %s\n", "libusb_set_configuration", libusb_strerror(ret));
		goto err;
	}

	//if (libusb_kernel_driver_active(dev->usb, 0))
	//	libusb_detach_kernel_driver(dev->usb, 0);

	ret = libusb_claim_interface(dev->usb, 0);
	if (ret) {
		fprintf(stderr, "%s: %s\n", "libusb_claim_interface", libusb_strerror(ret));
		goto err;
	}

	dev->transfer_out = libusb_alloc_transfer(0);
	if (!dev->transfer_out) {
		ret = LIBUSB_ERROR_NO_MEM;
		fprintf(stderr, "%s: %s\n", "libusb_alloc_transfer", libusb_strerror(ret));
		goto err;
	}

	dev->transfer_in = libusb_alloc_transfer(0);
	if (!dev->transfer_in) {
		ret = LIBUSB_ERROR_NO_MEM;
		fprintf(stderr, "%s: %s\n", "libusb_alloc_transfer", libusb_strerror(ret));
		goto err;
	}

	dev->cfg = malloc(HEADER_SIZE);
	if (!dev->cfg) {
		perror("malloc");
		goto err;
	}

	dev->frame_in = malloc(FRAME_PADDED_SIZE);
	if (!dev->frame_in) {
		perror("malloc");
		goto err;
	}

	dev->frame_done = malloc(FRAME_PADDED_SIZE);
	if (!dev->frame_done) {
		perror("malloc");
		goto err;
	}

#if __BYTE_ORDER == __LITTLE_ENDIAN
	memcpy(dev->cfg, &initial_cfg, HEADER_SIZE);
#else
	unsigned char *src = (unsigned char *)&initial_cfg;
	unsigned char *dst = dev->cfg;
	uint16_t word;
	for (size_t i = 0; i < HEADER_SIZE; i += sizeof word) {
		memcpy(&word, src, sizeof word);
		word = htole16(word);
		memcpy(dst, &word, sizeof word);
		src += sizeof word;
		dst += sizeof word;
	}
#endif
	return dev;

err:
	thermapp_usb_close(dev);
	return NULL;
}

// Create read and write thread
int
thermapp_usb_thread_create(struct thermapp_usb_dev *dev)
{
	int ret;

	dev->cond_frame_ready = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
	dev->mutex_frame_swap = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	dev->read_async_completed = 0;

	ret = pthread_create(&dev->pthread_read_async, NULL, read_async, (void *)dev);
	if (ret) {
		fprintf(stderr, "%s: %s\n", "pthread_create", strerror(ret));
		return -1;
	}
	dev->read_async_started = 1;

	return 0;
}

size_t
thermapp_usb_frame_read(struct thermapp_usb_dev *dev, void *buf, size_t len)
{
	if (len > FRAME_PADDED_SIZE) {
		len = FRAME_PADDED_SIZE;
	}
	len &= ~(sizeof (uint16_t) - 1);

	pthread_mutex_lock(&dev->mutex_frame_swap);
	pthread_cond_wait(&dev->cond_frame_ready, &dev->mutex_frame_swap);

	if (dev->read_async_completed) {
		len = 0;
	} else {
#if __BYTE_ORDER == __LITTLE_ENDIAN
		memcpy(buf, dev->frame_done, len);
#else
		// This assumes the data_offset / header_size is always even
		// therefore header and data combined is a stream of 16-bit little-endian values
		// TODO: Make it the caller's responsibility to handle endianness?
		unsigned char *src = dev->frame_done;
		unsigned char *dst = buf;
		uint16_t word;
		for (size_t i = 0; i < len; i += sizeof word) {
			memcpy(&word, src, sizeof word);
			word = le16toh(word);
			memcpy(dst, &word, sizeof word);
			src += sizeof word;
			dst += sizeof word;
		}
#endif
	}

	pthread_mutex_unlock(&dev->mutex_frame_swap);

	return len;
}

void
thermapp_usb_close(struct thermapp_usb_dev *dev)
{
	if (!dev)
		return;

	cancel_async(dev);

	if (dev->read_async_started) {
		pthread_join(dev->pthread_read_async, NULL);
	}

	libusb_free_transfer(dev->transfer_out);
	libusb_free_transfer(dev->transfer_in);

	if (dev->usb) {
		libusb_release_interface(dev->usb, 0);
		libusb_close(dev->usb);
	}

	if (dev->ctx) {
		libusb_exit(dev->ctx);
	}

	free(dev->frame_done);
	free(dev->frame_in);
	free(dev->cfg);
	free(dev);
}
