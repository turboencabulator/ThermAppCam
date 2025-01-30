/***************************************************************************
* Copyright (C) 2015 by Alexander G <pidbip@gmail.com>                     *
* Copyright (C) 2019 by Kyle Guinn <elyk03@gmail.com>                      *
*                                                                          *
* This program is free software: you can redistribute it and/or modify     *
* it under the terms of the GNU General Public License as published by     *
* the Free Software Foundation, either version 3 of the License, or        *
* (at your option) any later version.                                      *
*                                                                          *
* This program is distributed in the hope that it will be useful,          *
* but WITHOUT ANY WARRANTY; without even the implied warranty of           *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the             *
* GNU General Public License for more details.                             *
*                                                                          *
* You should have received a copy of the GNU General Public License        *
* along with this program. If not, see <http://www.gnu.org/licenses/>.     *
***************************************************************************/

#include "thermapp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct thermapp_cfg initial_cfg = {
	.preamble[0] = 0xa5a5,
	.preamble[1] = 0xa5a5,
	.preamble[2] = 0xa5a5,
	.preamble[3] = 0xa5d5,
	.modes       = 0x0002, //test pattern low
	//.serial_num_lo = 0,
	//.serial_num_hi = 0,
	//.hardware_ver = 0,
	//.firmware_ver = 0,
	.data_09     = FRAME_HEIGHT,
	.data_0a     = FRAME_WIDTH,
	.data_0b     = FRAME_HEIGHT,
	.data_0c     = FRAME_WIDTH,
	.data_0d     = 0x0019,
	.data_0e     = 0x0000,
	//.temperature = 0,
	.VoutA       = 0x075c,
	.data_11     = 0x0b85,
	.VoutC       = 0x05f4,
	.VoutD       = 0x0800,
	.VoutE       = 0x0b85,
	.data_15     = 0x0b85,
	.data_16     = 0x0000,
	.data_17     = 0x0570,
	.data_18     = 0x0b85,
	.data_19     = 0x0040,
	//.frame_count = 0,
	.data_1b     = 0x0000,
	.data_1c     = 0x0050,
	.data_1d     = 0x0003,
	.data_1e     = 0x0000,
	.data_1f     = 0x0fff,
};

struct thermapp_usb_dev *
thermapp_usb_open(void)
{
	struct thermapp_usb_dev *dev = calloc(1, sizeof *dev);
	if (!dev) {
		perror("calloc");
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

	memcpy(dev->cfg, &initial_cfg, HEADER_SIZE);
	return dev;

err:
	thermapp_usb_close(dev);
	return NULL;
}

int
thermapp_usb_connect(struct thermapp_usb_dev *dev)
{
	int ret;

	ret = libusb_init(&dev->ctx);
	if (ret) {
		fprintf(stderr, "libusb_init: %s\n", libusb_strerror(ret));
		return -1;
	}

	///FIXME: For Debug use libusb_open_device_with_vid_pid
	/// need to add search device
	dev->usb = libusb_open_device_with_vid_pid(dev->ctx, VENDOR, PRODUCT);
	if (!dev->usb) {
		ret = LIBUSB_ERROR_NO_DEVICE;
		fprintf(stderr, "libusb_open_device_with_vid_pid: %s\n", libusb_strerror(ret));
		return -1;
	}

	ret = libusb_set_configuration(dev->usb, 1);
	if (ret) {
		fprintf(stderr, "libusb_set_configuration: %s\n", libusb_strerror(ret));
		return -1;
	}

	//if (libusb_kernel_driver_active(dev->usb, 0))
	//	libusb_detach_kernel_driver(dev->usb, 0);

	ret = libusb_claim_interface(dev->usb, 0);
	if (ret) {
		fprintf(stderr, "libusb_claim_interface: %s\n", libusb_strerror(ret));
		return -1;
	}

	return 0;
}

static void
cancel_async(struct thermapp_usb_dev *dev, int due_to_transfer_error)
{
	if (dev->transfer_in) {
		int ret = libusb_cancel_transfer(dev->transfer_in);
		if (ret && ret != LIBUSB_ERROR_NOT_FOUND) {
			fprintf(stderr, "libusb_cancel_transfer: %s\n", libusb_strerror(ret));
		}
	}

	if (dev->transfer_out) {
		int ret = libusb_cancel_transfer(dev->transfer_out);
		if (ret && ret != LIBUSB_ERROR_NOT_FOUND) {
			fprintf(stderr, "libusb_cancel_transfer: %s\n", libusb_strerror(ret));
		}
	}

	if (due_to_transfer_error) {
		if (!dev->transfer_in && !dev->transfer_out) {
			// All transfers cancelled.
			// End the event loop and wake all waiters so they can exit.
			pthread_mutex_lock(&dev->mutex_frame_swap);
			dev->read_async_completed = 1;
			pthread_cond_broadcast(&dev->cond_frame_ready);
			pthread_mutex_unlock(&dev->mutex_frame_swap);
		}
	}
}

static void LIBUSB_CALL
transfer_cb_out(struct libusb_transfer *transfer)
{
	struct thermapp_usb_dev *dev = (struct thermapp_usb_dev *)transfer->user_data;

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		int ret = libusb_submit_transfer(transfer);
		if (ret) {
			fprintf(stderr, "libusb_submit_transfer: %s\n", libusb_strerror(ret));
		}
	} else if (transfer->status == LIBUSB_TRANSFER_ERROR
	        || transfer->status == LIBUSB_TRANSFER_NO_DEVICE
	        || transfer->status == LIBUSB_TRANSFER_CANCELLED) {
		libusb_free_transfer(dev->transfer_out);
		dev->transfer_out = NULL;

		cancel_async(dev, 1);
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
					if (memcmp(buf, &initial_cfg, sizeof initial_cfg.preamble) == 0) {
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
			fprintf(stderr, "libusb_submit_transfer: %s\n", libusb_strerror(ret));
		}
	} else if (transfer->status == LIBUSB_TRANSFER_ERROR
	        || transfer->status == LIBUSB_TRANSFER_NO_DEVICE
	        || transfer->status == LIBUSB_TRANSFER_CANCELLED) {
		libusb_free_transfer(dev->transfer_in);
		dev->transfer_in = NULL;

		cancel_async(dev, 1);
	}
}

static void *
read_async(void *ctx)
{
	struct thermapp_usb_dev *dev = (struct thermapp_usb_dev *)ctx;
	int ret;

	dev->transfer_out = libusb_alloc_transfer(0);
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
		fprintf(stderr, "libusb_submit_transfer: %s\n", libusb_strerror(ret));
		libusb_free_transfer(dev->transfer_out);
		dev->transfer_out = NULL;
	}

	dev->transfer_in = libusb_alloc_transfer(0);
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
		fprintf(stderr, "libusb_submit_transfer: %s\n", libusb_strerror(ret));
		libusb_free_transfer(dev->transfer_in);
		dev->transfer_in = NULL;
	}

	while (dev->transfer_out || dev->transfer_in) {
		ret = libusb_handle_events_completed(dev->ctx, &dev->read_async_completed);
		if (ret) {
			fprintf(stderr, "libusb_handle_events_completed: %s\n", libusb_strerror(ret));
			if (ret == LIBUSB_ERROR_INTERRUPTED) /* stray signal */ {
				continue;
			} else {
				break;
			}
		}
	}

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
		fprintf(stderr, "pthread_create: %s\n", strerror(ret));
		return -1;
	}
	dev->read_async_started = 1;

	return 0;
}

int
thermapp_usb_frame_read(struct thermapp_usb_dev *dev, void *buf, size_t len)
{
	int ret = 0;

	pthread_mutex_lock(&dev->mutex_frame_swap);
	pthread_cond_wait(&dev->cond_frame_ready, &dev->mutex_frame_swap);

	if (dev->read_async_completed || len > FRAME_PADDED_SIZE) {
		ret = -1;
	} else {
		memcpy(buf, dev->frame_done, len);
	}

	pthread_mutex_unlock(&dev->mutex_frame_swap);

	return ret;
}

void
thermapp_usb_close(struct thermapp_usb_dev *dev)
{
	if (!dev)
		return;

	cancel_async(dev, 0);

	if (dev->read_async_started) {
		pthread_join(dev->pthread_read_async, NULL);
	}

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
