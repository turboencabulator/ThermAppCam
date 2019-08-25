/***************************************************************************
* Copyright (C) 2015 by Alexander G  <pidbip@gmail.com>                    *
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
*                                                                          *
***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <time.h>
#include <string.h>
#include <errno.h>

#include "thermapp.h"


ThermApp *
thermapp_open(void)
{
	ThermApp *thermapp = calloc(1, sizeof *thermapp);
	if (!thermapp) {
		perror("calloc");
		return NULL;
	}

	thermapp->cfg = calloc(1, sizeof *thermapp->cfg);
	if (!thermapp->cfg) {
		perror("calloc");
		thermapp_close(thermapp);
		return NULL;
	}

	thermapp->therm_packet = malloc(sizeof *thermapp->therm_packet);
	if (!thermapp->therm_packet) {
		perror("malloc");
		thermapp_close(thermapp);
		return NULL;
	}

	//Initialize data struct
	// this init data was received from usbmonitor
	thermapp->cfg->preamble[0] = 0xa5a5;
	thermapp->cfg->preamble[1] = 0xa5a5;
	thermapp->cfg->preamble[2] = 0xa5a5;
	thermapp->cfg->preamble[3] = 0xa5d5;
	thermapp->cfg->modes = 0x0002; //test pattern low
	thermapp->cfg->data_09 = 0x0120;//
	thermapp->cfg->data_0a = 0x0180;//
	thermapp->cfg->data_0b = 0x0120;//
	thermapp->cfg->data_0c = 0x0180;// low
	thermapp->cfg->data_0d = 0x0019;// high
	thermapp->cfg->data_0e = 0x0000;//
	thermapp->cfg->VoutA = 0x0795;
	thermapp->cfg->data_11 = 0x0000;
	thermapp->cfg->VoutC = 0x058f;
	thermapp->cfg->VoutD = 0x08a2;
	thermapp->cfg->VoutE = 0x0b6d;
	thermapp->cfg->data_15 = 0x0b85;//
	thermapp->cfg->data_16 = 0x0000;//
	thermapp->cfg->data_17 = 0x0000;//
	thermapp->cfg->data_18 = 0x0998;//
	thermapp->cfg->data_19 = 0x0040;//
	thermapp->cfg->data_1b = 0x0000;//
	thermapp->cfg->data_1c = 0x0000;//low
	thermapp->cfg->data_1d = 0x0000;//
	thermapp->cfg->data_1e = 0x0000;//
	thermapp->cfg->data_1f = 0x0fff;//

	return thermapp;
}

int
thermapp_usb_connect(ThermApp *thermapp)
{
	int ret;

	ret = libusb_init(&thermapp->ctx);
	if (ret) {
		fprintf(stderr, "libusb_init: %s\n", libusb_strerror(ret));
		return -1;
	}

	///FIXME: For Debug use libusb_open_device_with_vid_pid
	/// need to add search device
	thermapp->dev = libusb_open_device_with_vid_pid(thermapp->ctx, VENDOR, PRODUCT);
	if (!thermapp->dev) {
		ret = LIBUSB_ERROR_NO_DEVICE;
		fprintf(stderr, "libusb_open_device_with_vid_pid: %s\n", libusb_strerror(ret));
		return -1;
	}

	ret = libusb_set_configuration(thermapp->dev, 1);
	if (ret) {
		fprintf(stderr, "libusb_set_configuration: %s\n", libusb_strerror(ret));
		return -1;
	}

	//if (libusb_kernel_driver_active(thermapp->dev, 0))
	//	libusb_detach_kernel_driver(thermapp->dev, 0);

	ret = libusb_claim_interface(thermapp->dev, 0);
	if (ret) {
		fprintf(stderr, "libusb_claim_interface: %s\n", libusb_strerror(ret));
		return -1;
	}

	return 0;
}

static void
thermapp_cancel_async(ThermApp *thermapp)
{
	if (thermapp->transfer_in) {
		int ret = libusb_cancel_transfer(thermapp->transfer_in);
		if (ret && ret != LIBUSB_ERROR_NOT_FOUND) {
			fprintf(stderr, "libusb_cancel_transfer: %s\n", libusb_strerror(ret));
		}
	}

	if (thermapp->transfer_out) {
		int ret = libusb_cancel_transfer(thermapp->transfer_out);
		if (ret && ret != LIBUSB_ERROR_NOT_FOUND) {
			fprintf(stderr, "libusb_cancel_transfer: %s\n", libusb_strerror(ret));
		}
	}
}

static void LIBUSB_CALL
transfer_cb_out(struct libusb_transfer *transfer)
{
	ThermApp *thermapp = (ThermApp *)transfer->user_data;

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		int ret = libusb_submit_transfer(transfer);
		if (ret) {
			fprintf(stderr, "libusb_submit_transfer: %s\n", libusb_strerror(ret));
		}
	} else if (transfer->status == LIBUSB_TRANSFER_ERROR
	        || transfer->status == LIBUSB_TRANSFER_NO_DEVICE
	        || transfer->status == LIBUSB_TRANSFER_CANCELLED) {
		libusb_free_transfer(thermapp->transfer_out);
		thermapp->transfer_out = NULL;

		thermapp_cancel_async(thermapp);
		thermapp->complete = thermapp->transfer_in == NULL;
	}
}

static void LIBUSB_CALL
transfer_cb_in(struct libusb_transfer *transfer)
{
	ThermApp *thermapp = (ThermApp *)transfer->user_data;

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		unsigned char *buf = transfer->buffer;
		size_t len = transfer->actual_length;
		while (len) {
			ssize_t w_len = write(thermapp->fd_pipe[1], buf, len);
			if (w_len < 0) {
				perror("pipe write");
				break;
			}
			buf += w_len;
			len -= w_len;
		}

		int ret = libusb_submit_transfer(transfer);
		if (ret) {
			fprintf(stderr, "libusb_submit_transfer: %s\n", libusb_strerror(ret));
		}
	} else if (transfer->status == LIBUSB_TRANSFER_ERROR
	        || transfer->status == LIBUSB_TRANSFER_NO_DEVICE
	        || transfer->status == LIBUSB_TRANSFER_CANCELLED) {
		free(thermapp->transfer_buf);
		thermapp->transfer_buf = NULL;
		libusb_free_transfer(thermapp->transfer_in);
		thermapp->transfer_in = NULL;

		thermapp_cancel_async(thermapp);
		thermapp->complete = thermapp->transfer_out == NULL;
	}
}

static void
thermapp_read_async(ThermApp *thermapp)
{
	fprintf(stderr, "thermapp_read_async\n");

	int ret;

	thermapp->transfer_out = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(thermapp->transfer_out,
	                          thermapp->dev,
	                          LIBUSB_ENDPOINT_OUT | 2,
	                          (unsigned char *)thermapp->cfg,
	                          sizeof *thermapp->cfg,
	                          transfer_cb_out,
	                          thermapp,
	                          0);
	ret = libusb_submit_transfer(thermapp->transfer_out);
	if (ret) {
		fprintf(stderr, "libusb_submit_transfer: %s\n", libusb_strerror(ret));
		libusb_free_transfer(thermapp->transfer_out);
		thermapp->transfer_out = NULL;
	}

	thermapp->transfer_buf = malloc(512);
	thermapp->transfer_in = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(thermapp->transfer_in,
	                          thermapp->dev,
	                          LIBUSB_ENDPOINT_IN | 1,
	                          thermapp->transfer_buf,
	                          512,
	                          transfer_cb_in,
	                          (void *)thermapp,
	                          BULK_TIMEOUT);
	ret = libusb_submit_transfer(thermapp->transfer_in);
	if (ret) {
		fprintf(stderr, "libusb_submit_transfer: %s\n", libusb_strerror(ret));
		free(thermapp->transfer_buf);
		thermapp->transfer_buf = NULL;
		libusb_free_transfer(thermapp->transfer_in);
		thermapp->transfer_in = NULL;
	}

	while (thermapp->transfer_out || thermapp->transfer_in) {
		ret = libusb_handle_events_completed(thermapp->ctx, &thermapp->complete);
		if (ret) {
			fprintf(stderr, "libusb_handle_events_completed: %s\n", libusb_strerror(ret));
			if (ret == LIBUSB_ERROR_INTERRUPTED) /* stray signal */ {
				continue;
			} else {
				break;
			}
		}
	}
}

static void *
thermapp_ThreadReadAsync(void *ctx)
{
	ThermApp *thermapp = (ThermApp *)ctx;

	puts("thermapp_ThreadReadAsync run");
	thermapp_read_async(thermapp);

	puts("close(thermapp->fd_pipe[1])");
	close(thermapp->fd_pipe[1]);
	thermapp->fd_pipe[1] = 0;

	return NULL;
}

static void *
thermapp_ThreadPipeRead(void *ctx)
{
	ThermApp *thermapp = (ThermApp *)ctx;
	struct cfg_packet header;
	size_t len;
	ssize_t ret;

	puts("thermapp_ThreadPipeRead run");
	while (!thermapp->complete) {

		for (len = 0; len < sizeof header; ) {
			if ((ret = read(thermapp->fd_pipe[0], (char *)&header + len, sizeof header - len)) <= 0) {
				perror("pipe read");
				break;
			}
			len += ret;
			//fprintf(stderr, "ret: %d, len: %d\n", ret, len);
		}

		// FIXME:  Assumes frame start is always 64-byte aligned.
		if (memcmp(&header, thermapp->cfg, sizeof header.preamble) == 0) {
			//fprintf(stderr, "FRAME_START\n");
			pthread_mutex_lock(&thermapp->mutex_thermapp);
			thermapp->therm_packet->header = header;
			for (len = sizeof header; len < sizeof *thermapp->therm_packet; ) {
				ret = read(thermapp->fd_pipe[0], (char *)thermapp->therm_packet + len, sizeof *thermapp->therm_packet - len);
				if (ret <= 0) {
					perror("pipe read");
					pthread_mutex_unlock(&thermapp->mutex_thermapp);
					break;
				}
				len += ret;
				//fprintf(stderr, "ret: %d, len: %d\n", ret, len);
			}

			if (1) { // FIXME:  Find a way to check the size.
				//fprintf(stderr, "FRAME_OK\n");
				pthread_cond_broadcast(&thermapp->cond_getimage);
			} else {
				fprintf(stderr, "lost frame\n");
				thermapp->lost_packet++; //increment damaged frames counter
			}

			pthread_mutex_unlock(&thermapp->mutex_thermapp);
		}
	}

	fprintf(stderr, "close(thermapp->fd_pipe[0]);\n");
	close(thermapp->fd_pipe[0]);
	thermapp->fd_pipe[0] = 0;

	return NULL;
}

// Create read and write thread
int
thermapp_thread_create(ThermApp *thermapp)
{
	int ret;

	thermapp->cond_getimage = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
	thermapp->mutex_thermapp = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	thermapp->complete = 0;

	ret = pipe(thermapp->fd_pipe);
	if (ret) {
		perror("pipe");
		return -1;
	}

	ret = pthread_create(&thermapp->pthreadReadAsync, NULL, thermapp_ThreadReadAsync, (void *)thermapp);
	if (ret) {
		fprintf(stderr, "pthread_create: %s\n", strerror(ret));
		return -1;
	}

	ret = pthread_create(&thermapp->pthreadReadPipe, NULL, thermapp_ThreadPipeRead, (void *)thermapp);
	if (ret) {
		fprintf(stderr, "pthread_create: %s\n", strerror(ret));
		return -1;
	}

	return 0;
}

int
thermapp_close(ThermApp *thermapp)
{
	if (!thermapp)
		return -1;

	thermapp_cancel_async(thermapp);

	sleep(1);

	if (thermapp->fd_pipe[0])
		close(thermapp->fd_pipe[0]);

	if (thermapp->fd_pipe[1])
		close(thermapp->fd_pipe[1]);

	if (thermapp->dev) {
		libusb_release_interface(thermapp->dev, 0);
		libusb_close(thermapp->dev);
	}

	if (thermapp->ctx) {
		libusb_exit(thermapp->ctx);
	}

	free(thermapp->therm_packet);
	free(thermapp->cfg);
	free(thermapp);

	return 0;
}

// This function for getting frame pixel data
void
thermapp_getImage(ThermApp *thermapp, int16_t *ImgData)
{
	pthread_mutex_lock(&thermapp->mutex_thermapp);
	pthread_cond_wait(&thermapp->cond_getimage, &thermapp->mutex_thermapp);

	thermapp->serial_num = thermapp->therm_packet->header.serial_num_lo
	                     | thermapp->therm_packet->header.serial_num_hi << 16;
	thermapp->hardware_ver = thermapp->therm_packet->header.hardware_ver;
	thermapp->firmware_ver = thermapp->therm_packet->header.firmware_ver;
	thermapp->temperature = thermapp->therm_packet->header.temperature;
	thermapp->frame_count = thermapp->therm_packet->header.frame_count;

	memcpy(ImgData, thermapp->therm_packet->pixels_data, PIXELS_DATA_SIZE*2);

	pthread_mutex_unlock(&thermapp->mutex_thermapp);
}

uint32_t
thermapp_getId(ThermApp *thermapp)
{
	return thermapp->serial_num;
}

//We don't know offset and quant value for temperature.
//We use experimental value.
float
thermapp_getTemperature(ThermApp *thermapp)
{
	return (thermapp->temperature - 14336) * 0.00652;
}

uint16_t
thermapp_getFrameCount(ThermApp *thermapp)
{
	return thermapp->frame_count;
}
