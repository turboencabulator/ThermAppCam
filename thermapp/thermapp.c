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


ThermApp *thermapp_initUSB(void)
{
	//fprintf(stderr, "malloc thermapp_initUSB\n");

	ThermApp *thermapp = malloc(sizeof *thermapp);
	if (!thermapp) {
		fprintf(stderr, "Can't allocate thermapp\n");
		return NULL;
	}

	memset(thermapp, 0, sizeof *thermapp);

	thermapp->cfg = malloc(sizeof *thermapp->cfg);
	if (!thermapp->cfg) {
		free(thermapp);
		fprintf(stderr, "Can't allocate cfg_packet\n");
		return NULL;
	}

	thermapp->therm_packet = malloc(sizeof *thermapp->therm_packet);
	if (!thermapp->therm_packet) {
		free(thermapp->cfg);
		free(thermapp);
		fprintf(stderr, "Can't allocate thermapp_packet\n");
		return NULL;
	}

	//Initialize data struct
	// this init data was received from usbmonitor
	thermapp->cfg->preamble[0] = 0xa5a5;
	thermapp->cfg->preamble[1] = 0xa5a5;
	thermapp->cfg->preamble[2] = 0xa5a5;
	thermapp->cfg->preamble[3] = 0xa5d5;
	thermapp->cfg->modes = 0x0002; //test pattern low
	thermapp->cfg->id_lo = 0;//
	thermapp->cfg->id_hi = 0;//
	thermapp->cfg->data_07 = 0x0000;//
	thermapp->cfg->data_08 = 0x0000;//
	thermapp->cfg->data_09 = 0x0120;//
	thermapp->cfg->data_0a = 0x0180;//
	thermapp->cfg->data_0b = 0x0120;//
	thermapp->cfg->data_0c = 0x0180;// low
	thermapp->cfg->data_0d = 0x0019;// high
	thermapp->cfg->data_0e = 0x0000;//
	thermapp->cfg->temperature = 0;//
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
	thermapp->cfg->frame_count = 0;//
	thermapp->cfg->data_1b = 0x0000;//
	thermapp->cfg->data_1c = 0x0000;//low
	thermapp->cfg->data_1d = 0x0000;//
	thermapp->cfg->data_1e = 0x0000;//
	thermapp->cfg->data_1f = 0x0fff;//

	thermapp->async_status = THERMAPP_INACTIVE;

	//Init libusb
	if (libusb_init(&thermapp->ctx) < 0) {
		free(thermapp->therm_packet);
		free(thermapp->cfg);
		free(thermapp);
		fprintf(stderr, "failed to initialize libusb\n");
		return NULL;
	}

	//fprintf(stderr, "PTHREAD INITIALIZER\n");

	//init thread condition
	thermapp->cond_pipe = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
	thermapp->cond_getimage = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
	thermapp->mutex_thermapp = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;

	//Make fifo pipe
	if (pipe(thermapp->fd_pipe) == -1) {
		free(thermapp->therm_packet);
		free(thermapp->cfg);
		free(thermapp);
		perror("pipe");
		return NULL;
	}
	//thermapp->is_NewFrame = FALSE;
	//thermapp->lost_packet = 0;

	return thermapp;
}

int thermapp_USB_checkForDevice(ThermApp *thermapp, int vendor, int product)
{
	int status;
	unsigned char buffer[255];

	//puts("libusb_open_device_with_vid_pid\n");

	///FIXME: For Debug use libusb_open_device_with_vid_pid
	/// need to add search device
	thermapp->dev = libusb_open_device_with_vid_pid(thermapp->ctx, vendor, product);
	if (!thermapp->dev) {
		free(thermapp->cfg);
		free(thermapp);
		fprintf(stderr, "Open device with vid pid failed\n");
		return -1;
	}

	//if (libusb_kernel_driver_active(thermapp->dev, 0))
	//	libusb_detach_kernel_driver(thermapp->dev, 0);

	if (libusb_claim_interface(thermapp->dev, 0)) {
		free(thermapp->cfg);
		free(thermapp);
		fprintf(stderr, "claim interface failed\n");
		return 0;
	}


	// We don't know what is this but this is needed to make ThermApp work. We received it from usbmonitor
	status = libusb_control_transfer(thermapp->dev, LIBUSB_ENDPOINT_IN, 0x06, 0x0100, 0x0000, buffer, 0x12, 0);
	fprintf(stdout, "status: %d, ", status);

	status = libusb_control_transfer(thermapp->dev, LIBUSB_ENDPOINT_IN, 0x06, 0x0200, 0x0000, buffer, 0x09, 0);
	fprintf(stdout, "status: %d, ", status);

	status = libusb_control_transfer(thermapp->dev, LIBUSB_ENDPOINT_IN, 0x06, 0x0200, 0x0000, buffer, 0x20, 0);
	fprintf(stdout, "status: %d, ", status);

	status = libusb_control_transfer(thermapp->dev, LIBUSB_ENDPOINT_IN, 0x06, 0x0300, 0x0000, buffer, 0xff, 0);
	fprintf(stdout, "status: %d, ", status);

	status = libusb_control_transfer(thermapp->dev, LIBUSB_ENDPOINT_IN, 0x06, 0x0302, 0x0409, buffer, 0xff, 0);
	fprintf(stdout, "status: %d, ", status);

	status = libusb_control_transfer(thermapp->dev, LIBUSB_ENDPOINT_IN, 0x06, 0x0301, 0x0409, buffer, 0xff, 0);
	fprintf(stdout, "status: %d, ", status);

	status = libusb_control_transfer(thermapp->dev, LIBUSB_ENDPOINT_IN, 0x06, 0x0303, 0x0409, buffer, 0xff, 0);
	fprintf(stdout, "status: %d, ", status);

	status = libusb_control_transfer(thermapp->dev, LIBUSB_TRANSFER_TYPE_CONTROL, 0x09, 0x0001, 0x0000, buffer, 0x00, 0);
	fprintf(stdout, "status: %d, ", status);

	status = libusb_control_transfer(thermapp->dev, LIBUSB_ENDPOINT_IN, 0x06, 0x0304, 0x0409, buffer, 0xff, 0);
	fprintf(stdout, "status: %d, ", status);
	//print_status(buffer, 0xff);

	status = libusb_control_transfer(thermapp->dev, LIBUSB_ENDPOINT_IN, 0x06, 0x0305, 0x0409, buffer, 0xff, 0);
	fprintf(stdout, "status: %d, \n", status);

	return 0;
}

static
void THERMAPP_CALL thermapp_PipeWrite(unsigned char *buf, uint32_t len, void *ctx)
{
	//fprintf(stderr, "therm_callback\n");
	unsigned int w_len;

	if (len && ctx) {
		if ((w_len = write(*(int *)ctx, buf, len)) <= 0) {
			perror("fifo write");
		}
		//FIXME: w_len return can be smaller len
		if (w_len < len) {
			printf("pipe write len smaller. len: %d w_len: %d\n", len, w_len);
		}
	}
}

void *thermapp_ThreadReadAsync(void *ctx)
{
	ThermApp *thermapp = ctx;

	puts("thermapp_ThreadReadAsync run\n");
	thermapp_read_async(thermapp, thermapp_PipeWrite, (void *)&thermapp->fd_pipe[1]);

	puts("close(thermapp->fd_pipe[1])\n");

	close(thermapp->fd_pipe[1]);
	thermapp->fd_pipe[1] = 0;

	return NULL;
}

void *thermapp_ThreadPipeRead(void *ctx)
{
	ThermApp *thermapp = (ThermApp *)ctx;
	struct cfg_packet header;
	size_t len;
	ssize_t ret;

	enum thermapp_async_status current_status = THERMAPP_RUNNING;

	puts("thermapp_ThreadPipeRead run\n");
	while (current_status == THERMAPP_RUNNING) {

		for (len = 0; len < sizeof header; ) {
			if ((ret = read(thermapp->fd_pipe[0], (char *)&header + len, sizeof header - len)) <= 0) {
				fprintf(stderr, "read thermapp_ThreadPipeRead()\n");
				perror("fifo pipe read");
				break;
			}
			len += ret;
			//fprintf(stderr, "ret: %d, len: %d\n", ret, len);
		}

		//fprintf(stderr, "thermapp_ThreadPipeRead(): thermapp->async_status =  %d\n", thermapp->async_status);

		// FIXME:  Assumes frame start is always 64-byte aligned.
		if (memcmp(&header, thermapp->cfg, sizeof header.preamble) == 0) {
			//fprintf(stderr, "FRAME_START\n");
			thermapp->is_NewFrame = FALSE;
			pthread_mutex_lock(&thermapp->mutex_thermapp);
			thermapp->therm_packet->header = header;
			for (len = sizeof header; len < sizeof *thermapp->therm_packet; ) {
				ret = read(thermapp->fd_pipe[0], (char *)thermapp->therm_packet + len, sizeof *thermapp->therm_packet - len);
				if (ret <= 0) {
					fprintf(stderr, "read thermapp_ThreadPipeRead()\n");
					perror("fifo pipe read");
					pthread_mutex_unlock(&thermapp->mutex_thermapp);
					break;
				}
				len += ret;
				//fprintf(stderr, "ret: %d, len: %d\n", ret, len);
			}

			if (1) { // FIXME:  Find a way to check the size.
				//fprintf(stderr, "FRAME_OK\n");
				thermapp->is_NewFrame = TRUE;
				//pthread_cond_signal(&thermapp->cond_getimage);
				pthread_cond_wait(&thermapp->cond_pipe, &thermapp->mutex_thermapp);
			} else {
				fprintf(stderr, "lost frame\n");
				thermapp->lost_packet++; //increment damaged frames counter
			}

			current_status = thermapp->async_status;
			pthread_mutex_unlock(&thermapp->mutex_thermapp);
		}
	}

	fprintf(stderr, "close(thermapp->fd_pipe[0]);\n");

	close(thermapp->fd_pipe[0]);
	thermapp->fd_pipe[0] = 0;

	return NULL;
}

int thermapp_ParsingUsbPacket(ThermApp *thermapp, short *ImgData)
{
	thermapp->id = thermapp->therm_packet->header.id_lo
	             | thermapp->therm_packet->header.id_hi << 16;
	thermapp->temperature = thermapp->therm_packet->header.temperature;
	thermapp->frame_count = thermapp->therm_packet->header.frame_count;

#if 0
	int i;
	for (i = 0; i < PIXELS_DATA_SIZE; i++) {
		ImgData[i] = thermapp->therm_packet->pixels_data[i];
		         //- thermapp->calibrate_pixels[0][i];
	}
#else

#if 1
	memcpy(ImgData, thermapp->therm_packet->pixels_data, PIXELS_DATA_SIZE*2);
#else
	// Debug some test
	int i, i_src = 0;
	for (i = 0; i < PIXELS_DATA_SIZE; i++) {
		//if ((i == 8567))
		//else
		ImgData[i] = thermapp->therm_packet->pixels_data[i_src];
		i_src++;
	}
	ImgData[8567] = (thermapp->therm_packet->pixels_data[8566]
	               + thermapp->therm_packet->pixels_data[8568])/2;

	i_src = 63360-384;
	for (i = 63360; i < 63360+384; i++) {
		ImgData[i] = thermapp->therm_packet->pixels_data[i_src];
		i_src++;
	}

	i_src = 79488-384;
	for (i = 79488; i < 79488+384; i++) {
		ImgData[i] = thermapp->therm_packet->pixels_data[i_src];
		i_src++;
	}
#endif
#endif

	return 1;
}

// This function for getting frame pixel data
int thermapp_GetImage(ThermApp *thermapp, short *ImgData)
{
	if (!thermapp->is_NewFrame) return 0;

	pthread_mutex_lock(&thermapp->mutex_thermapp);

	thermapp_ParsingUsbPacket(thermapp, ImgData);
	thermapp->is_NewFrame = FALSE;
	pthread_cond_signal(&thermapp->cond_pipe);

	pthread_mutex_unlock(&thermapp->mutex_thermapp);

	return 1;
}


// Create read and write thread
int thermapp_FrameRequest_thread(ThermApp *thermapp)
{
	int  ret;

	ret = pthread_create(&thermapp->pthreadReadAsync, NULL, thermapp_ThreadReadAsync, (void *)thermapp);
	if (ret) {
		fprintf(stderr, "Error - pthread_create() return code: %d\n", ret);
		return -1;
	}

	ret = pthread_create(&thermapp->pthreadReadPipe, NULL, thermapp_ThreadPipeRead, (void *)thermapp);
	if (ret) {
		fprintf(stderr, "Error - pthread_create() return code: %d\n", ret);
		return -1;
	}

	return 1;
}


/*
void thermapp_setGain(ThermApp *thermapp, unsigned short gain)
{
	//thermapp->cfg->gain = gain;
}

unsigned short thermapp_getGain(ThermApp *thermapp)
{
	//return thermapp->cfg->gain;
}
*/

unsigned int thermapp_getId(ThermApp *thermapp)
{
	return thermapp->id;
}

//We don't know offset and quant value for temperature.
//We use experimental value.
float thermapp_getTemperature(ThermApp *thermapp)
{
	short t = thermapp->temperature;
	return (t - 14336) * 0.00652;
}

unsigned short thermapp_getFrameCount(ThermApp *thermapp)
{
	return thermapp->frame_count;
}

/*
unsigned short thermapp_getDCoffset(ThermApp *thermapp)
{
	//return thermapp->cfg->DCoffset;
}

void thermapp_setDCoffset(ThermApp *thermapp, unsigned short offset)
{
	//thermapp->cfg->DCoffset = offset;
}
*/

/*
int thermapp_LoadCalibrate(ThermApp *thermapp, unsigned int id)
{
	return 0;
}
*/

//Transfer function for put date to ThermApp
static void LIBUSB_CALL transfer_cb_out(struct libusb_transfer *transfer)
{
	ThermApp *thermapp = transfer->user_data;

	if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		fprintf(stderr, "transfer_cb_out() : in transfer status %d\n", transfer->status);
		libusb_free_transfer(transfer);
		thermapp->transfer_out = NULL;
		return;
	}

	if (libusb_submit_transfer(thermapp->transfer_out) < 0) {
		fprintf(stderr, "in transfer libusb_submit_transfer\n");
	}

}

static
void LIBUSB_CALL _libusb_callback(struct libusb_transfer *transfer)
{
	//fprintf(stderr, "LIBUSB_CALL _libusb_callback\n");

	ThermApp *thermapp = (ThermApp *)transfer->user_data;

	if (LIBUSB_TRANSFER_COMPLETED == transfer->status) {
		//fprintf(stderr, "LIBUSB_TRANSFER_COMPLETED\n");
		if (thermapp->cb)
			thermapp->cb(transfer->buffer, transfer->actual_length, thermapp->cb_ctx);
		//write(thermapp->fd_pipe[1], transfer->buffer, transfer->actual_length);// Write to fifo pipe

		libusb_submit_transfer(transfer); /* resubmit transfer */
	} else if (LIBUSB_TRANSFER_ERROR == transfer->status
	        || LIBUSB_TRANSFER_NO_DEVICE == transfer->status) {
		thermapp->dev_lost = 1;
		thermapp_cancel_async(thermapp);
		fprintf(stderr, "LIBUSB_TRANSFER_ERROR or LIBUSB_TRANSFER_NO_DEVICE\n");
	}
}

int thermapp_read_async(ThermApp *thermapp, thermapp_read_async_cb_t cb, void *ctx)
{
	fprintf(stderr, "thermapp_read_async\n");

	int r = 0;
	struct timeval tv = { 1, 0 };
	struct timeval zerotv = { 0, 0 };
	enum thermapp_async_status next_status = THERMAPP_INACTIVE;

	if (!thermapp) {
		fprintf(stderr, "!thermapp\n");
		return -1;
	}

	if (THERMAPP_INACTIVE != thermapp->async_status) {
		fprintf(stderr, "THERMAPP_INACTIVE != thermapp->async_status\n");
		return -2;
	}

	thermapp->async_status = THERMAPP_RUNNING;
	thermapp->async_cancel = 0;

	thermapp->cb = cb;
	thermapp->cb_ctx = ctx;

	thermapp->transfer_out = libusb_alloc_transfer(0);
	thermapp->transfer_in = libusb_alloc_transfer(0);
	thermapp->transfer_buf = malloc((sizeof *thermapp->therm_packet + 511) & ~511);

	libusb_fill_bulk_transfer(thermapp->transfer_out,
	                          thermapp->dev,
	                          LIBUSB_ENDPOINT_OUT | 2,
	                          (unsigned char *)thermapp->cfg,
	                          sizeof *thermapp->cfg,
	                          transfer_cb_out,
	                          thermapp,
	                          0);
	r = libusb_submit_transfer(thermapp->transfer_out);

	libusb_fill_bulk_transfer(thermapp->transfer_in,
	                          thermapp->dev,
	                          LIBUSB_ENDPOINT_IN | 1,
	                          thermapp->transfer_buf,
	                          (sizeof *thermapp->therm_packet + 511) & ~511,
	                          _libusb_callback,
	                          (void *)thermapp,
	                          BULK_TIMEOUT);
	r = libusb_submit_transfer(thermapp->transfer_in);
	if (r < 0) {
		fprintf(stderr, "Failed to submit transfer!\n");
		thermapp->async_status = THERMAPP_CANCELING;
	}

	while (THERMAPP_INACTIVE != thermapp->async_status) {
		r = libusb_handle_events_timeout_completed(thermapp->ctx, &tv, &thermapp->async_cancel);
		//r = libusb_handle_events_completed(thermapp->ctx, &thermapp->async_cancel);
		if (r < 0) {
			fprintf(stderr, "handle_events returned: %d\n", r);
			if (r == LIBUSB_ERROR_INTERRUPTED) /* stray signal */ {
				fprintf(stderr, "LIBUSB_ERROR_INTERRUPTED\n");
				continue;
			}
			break;
		}

		if (THERMAPP_CANCELING == thermapp->async_status) {
			fprintf(stderr, "THERMAPP_CANCELING == thermapp->async_status\n");

			next_status = THERMAPP_INACTIVE;

			if (!thermapp->transfer_in)
				break;

			if (LIBUSB_TRANSFER_CANCELLED != thermapp->transfer_in->status) {
				r = libusb_cancel_transfer(thermapp->transfer_in);
				// handle events after cancelling
				// to allow transfer status to
				// propagate
				libusb_handle_events_timeout_completed(thermapp->ctx, &zerotv, NULL);
				if (r == 0)
					next_status = THERMAPP_CANCELING;
			}

			if (thermapp->dev_lost || THERMAPP_INACTIVE == next_status) {
				// handle any events that still need to
				// be handled before exiting after we
				// just cancelled all transfers
				libusb_handle_events_timeout_completed(thermapp->ctx, &zerotv, NULL);
				break;
			}
		}
	}
	if (THERMAPP_INACTIVE == thermapp->async_status)
		fprintf(stderr, "THERMAPP_INACTIVE == thermapp->async_status\n");

	libusb_free_transfer(thermapp->transfer_out);
	thermapp->transfer_out = NULL;

	libusb_free_transfer(thermapp->transfer_in);
	thermapp->transfer_in = NULL;

	free(thermapp->transfer_buf);
	thermapp->transfer_buf = NULL;

	thermapp->async_status = next_status;

	return r;
}

int thermapp_cancel_async(ThermApp *thermapp)
{
	if (!thermapp)
		return -1;

	// if streaming, try to cancel gracefully
	if (THERMAPP_RUNNING == thermapp->async_status) {
		thermapp->async_status = THERMAPP_CANCELING;
		thermapp->async_cancel = 1;
		return 0;
	}

	// if called while in pending state, change the state forcefully
#if 0
	if (THERMAPP_INACTIVE != thermapp->async_status) {
		thermapp->async_status = THERMAPP_INACTIVE;
		return 0;
	}
#endif
	return -2;
}


int thermapp_Close(ThermApp *thermapp)
{
	if (!thermapp)
		return -1;

	thermapp_cancel_async(thermapp);

	sleep(1);

	if (thermapp->fd_pipe[0])
		close(thermapp->fd_pipe[0]);
	thermapp->fd_pipe[0] = 0;

	if (thermapp->fd_pipe[1])
		close(thermapp->fd_pipe[1]);
	thermapp->fd_pipe[1] = 0;

	free(thermapp->cfg);
	thermapp->cfg = NULL;

	libusb_release_interface(thermapp->dev, 0);

	//libusb_close(thermapp->dev);
	//printf("libusb_close\n");
	libusb_exit(thermapp->ctx);

	free(thermapp);
	thermapp = NULL;

	return 0;
}
