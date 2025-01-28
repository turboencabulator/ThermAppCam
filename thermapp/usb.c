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

#define ROUND_UP_512(num) (((num)+511)&~511)

struct thermapp_usb_dev *
thermapp_usb_open(void)
{
	struct thermapp_usb_dev *dev = calloc(1, sizeof *dev);
	if (!dev) {
		perror("calloc");
		goto err1;
	}

	dev->cfg = calloc(1, sizeof *dev->cfg);
	if (!dev->cfg) {
		perror("calloc");
		goto err2;
	}

	dev->frame_in = malloc(ROUND_UP_512(sizeof *dev->frame_in));
	if (!dev->frame_in) {
		perror("malloc");
		goto err2;
	}

	dev->frame_done = malloc(ROUND_UP_512(sizeof *dev->frame_done));
	if (!dev->frame_done) {
		perror("malloc");
		goto err2;
	}

	//Initialize data struct
	// this init data was received from usbmonitor
	dev->cfg->preamble[0] = 0xa5a5;
	dev->cfg->preamble[1] = 0xa5a5;
	dev->cfg->preamble[2] = 0xa5a5;
	dev->cfg->preamble[3] = 0xa5d5;
	dev->cfg->modes = 0x0002; //test pattern low
	dev->cfg->data_09 = FRAME_HEIGHT;
	dev->cfg->data_0a = FRAME_WIDTH;
	dev->cfg->data_0b = FRAME_HEIGHT;
	dev->cfg->data_0c = FRAME_WIDTH;
	dev->cfg->data_0d = 0x0019;
	dev->cfg->data_0e = 0x0000;
	dev->cfg->VoutA = 0x075c;
	dev->cfg->data_11 = 0x0b85;
	dev->cfg->VoutC = 0x05f4;
	dev->cfg->VoutD = 0x0800;
	dev->cfg->VoutE = 0x0b85;
	dev->cfg->data_15 = 0x0b85;
	dev->cfg->data_16 = 0x0000;
	dev->cfg->data_17 = 0x0570;
	dev->cfg->data_18 = 0x0b85;
	dev->cfg->data_19 = 0x0040;
	dev->cfg->data_1b = 0x0000;
	dev->cfg->data_1c = 0x0050;
	dev->cfg->data_1d = 0x0003;
	dev->cfg->data_1e = 0x0000;
	dev->cfg->data_1f = 0x0fff;

	return dev;

err2:
	thermapp_usb_close(dev);
err1:
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
		// Device apparently only works with 512-byte chunks of data.
		// Note the frame is padded to a multiple of 512 bytes.
		if (transfer->actual_length % 512) {
			fprintf(stderr, "discarding partial transfer of size %u\n", transfer->actual_length);
			transfer->buffer = (unsigned char *)dev->frame_in;
			transfer->length = TRANSFER_SIZE;
		} else if (transfer->actual_length) {
			unsigned char *buf = (unsigned char *)dev->frame_in;
			size_t old = (unsigned char *)transfer->buffer - buf;
			size_t len = old + transfer->actual_length;

			if (!old) {
				// Sync to start of frame.
				// Look for preamble at start of 512-byte chunk.
				while (len >= 512) {
					if (memcmp(buf, dev->cfg, sizeof dev->cfg->preamble) == 0) {
						break;
					}
					buf += 512;
					len -= 512;
				}
				memmove(dev->frame_in, buf, len);
			}

			if (len == ROUND_UP_512(sizeof *dev->frame_in)) {
				// Frame complete.
				pthread_mutex_lock(&dev->mutex_frame_swap);
				struct thermapp_frame *tmp = dev->frame_done;
				dev->frame_done = dev->frame_in;
				dev->frame_in = tmp;
				pthread_cond_broadcast(&dev->cond_frame_ready);
				pthread_mutex_unlock(&dev->mutex_frame_swap);

				transfer->buffer = (unsigned char *)dev->frame_in;
				len = 0;
			}

			transfer->buffer = (unsigned char *)dev->frame_in + len;
			transfer->length = TRANSFER_SIZE;
			if (transfer->length > ROUND_UP_512(sizeof *dev->frame_in) - len) {
				transfer->length = ROUND_UP_512(sizeof *dev->frame_in) - len;
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
	                          (unsigned char *)dev->cfg,
	                          sizeof *dev->cfg,
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
	                          (unsigned char *)dev->frame_in,
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
thermapp_usb_frame_read(struct thermapp_usb_dev *dev, struct thermapp_frame *frame)
{
	int ret = 0;

	pthread_mutex_lock(&dev->mutex_frame_swap);
	pthread_cond_wait(&dev->cond_frame_ready, &dev->mutex_frame_swap);

	if (dev->read_async_completed) {
		ret = -1;
	} else {
		memcpy(frame, dev->frame_done, sizeof *dev->frame_done);
	}

	pthread_mutex_unlock(&dev->mutex_frame_swap);

	return ret;
}

int
thermapp_usb_close(struct thermapp_usb_dev *dev)
{
	if (!dev)
		return -1;

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

	return 0;
}
