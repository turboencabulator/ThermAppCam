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
static const union thermapp_cfg initial_cfg = {
	.preamble[0] = 0xa5a5,
	.preamble[1] = 0xa5a5,
	.preamble[2] = 0xa5a5,
	.preamble[3] = 0xa5d5,
	.modes       = 0x0002, // (control) // test pattern low
	//.serial_num_lo = 0, // (status)
	//.serial_num_hi = 0, // (status)
	//.hardware_num = 0, // (status)
	//.firmware_num = 0, // (status)
	//.fpa_h       = 0, // (status)
	//.fpa_w       = 0, // (status)
	.data_h      = FRAME_HEIGHT_MAX, // (control/status)
	.data_w      = FRAME_WIDTH_MAX, // (control/status)
	.data_0d     = 0x0019,
	//.temp_thermistor = 0, // (status)
	//.temp_fpa_diode = 0, // (status)
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
	//.frame_num_lo = 0, // (status)
	//.frame_num_hi = 0, // (status)
	.data_1c     = 0x0050,
	.data_1d     = 0x0003,
	.data_1e     = 0x0000,
	.data_1f     = 0x0fff,
};

static size_t
sync(unsigned char *buf)
{
	// Frame must begin with the header preamble.
	if (memcmp(buf, preamble, sizeof preamble) != 0) {
		return 0;
	}

	// Convert selected header fields to native byte order.
	size_t fpa_h       = buf[0x12] + (buf[0x13] << 8);
	size_t fpa_w       = buf[0x14] + (buf[0x15] << 8);
	size_t data_h      = buf[0x16] + (buf[0x17] << 8);
	size_t data_w      = buf[0x18] + (buf[0x19] << 8);
	size_t data_offset = buf[0x32] + (buf[0x33] << 8);

	// Sanity check:  Reject unexpected frame size values.  Guarantees that:
	// * Header size / data offset is exactly 64 bytes.
	//   * Image data immediately follows the header with no overlap or gap.
	//   * Image data begins on an even byte boundary for endian conversions.
	// * Image is no larger than 640x480.
	//   * Establishes the minimum buffer size to store the largest frame.
	//   * Image can only be larger than the FPA in the special case below.
	if (!((fpa_w == 384 && fpa_h == 288)
	   || (fpa_w == 640 && fpa_h == 480))
	 || data_w > fpa_w
	 || data_h > fpa_h
	 || data_offset != HEADER_SIZE) {
		return 0;
	}

	// Special cases where the reported size is incorrect.
	// Rewrite the header so that users don't need to know this.
	// XXX: May be model-specific or firmware-specific behavior.
	//      Tested on original ThermApp (HW #4, FW #120).
	if (!data_w && !data_h) {
		data_w = 512;
		data_h = 308;
		buf[0x16] = data_h      & 0xff;
		buf[0x17] = data_h >> 8 & 0xff;
		buf[0x18] = data_w      & 0xff;
		buf[0x19] = data_w >> 8 & 0xff;
	} else if (data_w < FRAME_WIDTH_MIN || data_h < FRAME_HEIGHT_MIN) {
		data_w = fpa_w;
		data_h = fpa_h;
		memcpy(&buf[0x16], &buf[0x12], 4);
	}

	return data_offset + 2 * data_w * data_h;
}

static void
cancel_transfers(struct thermapp_usb_dev *dev)
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

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		if (dev->cfg_fill_sz) {
			memcpy(dev->cfg_out, dev->cfg_fill, dev->cfg_fill_sz);

			transfer->buffer = dev->cfg_fill;
			dev->cfg_fill = dev->cfg_out;
			dev->cfg_out = transfer->buffer;
			transfer->length = dev->cfg_fill_sz;
			dev->cfg_fill_sz = 0;

			int ret = libusb_submit_transfer(transfer);
			if (ret) {
				fprintf(stderr, "%s: %s\n", "libusb_submit_transfer", libusb_strerror(ret));
				transfer->buffer = NULL;
			}
		} else {
			transfer->buffer = NULL;
		}
	} else {
		transfer->buffer = NULL;
		cancel_transfers(dev);
	}
}

static void LIBUSB_CALL
transfer_cb_in(struct libusb_transfer *transfer)
{
	struct thermapp_usb_dev *dev = (struct thermapp_usb_dev *)transfer->user_data;

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		if (transfer->actual_length % PACKET_SIZE) {
			fprintf(stderr, "discarding partial transfer of size %u\n", transfer->actual_length);
			transfer->buffer = dev->frame_in;
			transfer->length = BULK_SIZE_MIN;
		} else if (transfer->actual_length) {
			unsigned char *buf = dev->frame_in;
			size_t exp = dev->frame_in_sz;
			size_t old = (unsigned char *)transfer->buffer - buf;
			size_t len = old + transfer->actual_length;

			if (!old) {
				// No previous data.  Sync to start of frame.
				do {
					// Need at least HEADER_SIZE bytes to sync.
					// len is a nonzero multiple of PACKET_SIZE bytes.
					exp = sync(buf);
					if (exp) {
						dev->frame_in_sz = exp;
						memmove(dev->frame_in, buf, len);
						break;
					}
					buf += PACKET_SIZE;
					len -= PACKET_SIZE;
				} while (len);
			}

			if (!len) {
				// Still not sync'd.
				transfer->buffer = dev->frame_in;
				transfer->length = BULK_SIZE_MIN;
			} else if (len < exp) {
				// Partially received.  Request the remainder.
				transfer->buffer = dev->frame_in + len;
				transfer->length = (exp - len + PACKET_SIZE - 1) & ~(PACKET_SIZE - 1);
			} else {
				// Frame complete.  Discard any excess.
				transfer->buffer = dev->frame_done;
				dev->frame_done = dev->frame_in;
				dev->frame_in = transfer->buffer;
				dev->frame_done_sz = exp;

				// Resync.  The next frame may not be the same size.
				transfer->length = BULK_SIZE_MIN;
			}
		}

		int ret = libusb_submit_transfer(transfer);
		if (ret) {
			fprintf(stderr, "%s: %s\n", "libusb_submit_transfer", libusb_strerror(ret));
			transfer->buffer = NULL;
		}
	} else {
		transfer->buffer = NULL;
		cancel_transfers(dev);
	}
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

	dev->cfg_fill = malloc(HEADER_SIZE);
	if (!dev->cfg_fill) {
		perror("malloc");
		goto err;
	}

	dev->cfg_out = malloc(HEADER_SIZE);
	if (!dev->cfg_out) {
		perror("malloc");
		goto err;
	}

	dev->frame_in = malloc(BULK_SIZE_MAX);
	if (!dev->frame_in) {
		perror("malloc");
		goto err;
	}

	dev->frame_done = malloc(BULK_SIZE_MAX);
	if (!dev->frame_done) {
		perror("malloc");
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
	libusb_fill_bulk_transfer(dev->transfer_out,
	                          dev->usb,
	                          LIBUSB_ENDPOINT_OUT | 2,
	                          NULL, //dev->cfg_out,
	                          HEADER_SIZE,
	                          transfer_cb_out,
	                          dev,
	                          0);

	dev->transfer_in = libusb_alloc_transfer(0);
	if (!dev->transfer_in) {
		ret = LIBUSB_ERROR_NO_MEM;
		fprintf(stderr, "%s: %s\n", "libusb_alloc_transfer", libusb_strerror(ret));
		goto err;
	}
	libusb_fill_bulk_transfer(dev->transfer_in,
	                          dev->usb,
	                          LIBUSB_ENDPOINT_IN | 1,
	                          NULL, //dev->frame_in,
	                          BULK_SIZE_MIN,
	                          transfer_cb_in,
	                          (void *)dev,
	                          0);

	return dev;

err:
	thermapp_usb_close(dev);
	return NULL;
}

void
thermapp_usb_start(struct thermapp_usb_dev *dev)
{
	dev->transfer_in->buffer = dev->frame_in;
	dev->transfer_in->status = LIBUSB_TRANSFER_COMPLETED;
	dev->transfer_in->actual_length = 0;
	transfer_cb_in(dev->transfer_in);

	thermapp_usb_cfg_write(dev, &initial_cfg, 0, sizeof initial_cfg);
}

int
thermapp_usb_transfers_pending(struct thermapp_usb_dev *dev)
{
	// Using transfer->buffer as an indication that transfers are pending.
	return dev->transfer_out->buffer || dev->transfer_in->buffer;
}

void
thermapp_usb_handle_events(struct thermapp_usb_dev *dev)
{
	int ret = libusb_handle_events(dev->ctx);
	if (ret) {
		fprintf(stderr, "%s: %s\n", "libusb_handle_events", libusb_strerror(ret));
	}
}

size_t
thermapp_usb_frame_read(struct thermapp_usb_dev *dev, void *buf, size_t len)
{
	if (len > dev->frame_done_sz) {
		len = dev->frame_done_sz;
	}
	len &= ~(sizeof (uint16_t) - 1);

	if (len) {
		dev->frame_done_sz = 0;
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

	return len;
}

size_t
thermapp_usb_cfg_write(struct thermapp_usb_dev *dev, const void *buf, size_t ofs, size_t len)
{
	if (ofs >= HEADER_SIZE
	 || len > HEADER_SIZE - ofs
	 || ofs & (sizeof (uint16_t) - 1)
	 || len & (sizeof (uint16_t) - 1)) {
		return 0;
	}

	if (len) {
		dev->cfg_fill_sz = 0;
#if __BYTE_ORDER == __LITTLE_ENDIAN
		memcpy(dev->cfg_fill + ofs, buf, len);
#else
		// This assumes ofs and len are always even
		// TODO: Make it the caller's responsibility to handle endianness?
		unsigned char *src = buf;
		unsigned char *dst = dev->cfg_fill + ofs;
		uint16_t word;
		for (size_t i = 0; i < len; i += sizeof word) {
			memcpy(&word, src, sizeof word);
			word = htole16(word);
			memcpy(dst, &word, sizeof word);
			src += sizeof word;
			dst += sizeof word;
		}
#endif
	} else {
		// Partial writes are buffered until completed with a 0-length write.
		len = HEADER_SIZE;
	}

	if (len == HEADER_SIZE) {
		dev->cfg_fill_sz = len;
		if (!dev->transfer_out->buffer) {
			dev->transfer_out->status = LIBUSB_TRANSFER_COMPLETED;
			transfer_cb_out(dev->transfer_out);
		}
	}

	return len;
}

void
thermapp_usb_close(struct thermapp_usb_dev *dev)
{
	if (!dev)
		return;

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
	free(dev->cfg_out);
	free(dev->cfg_fill);
	free(dev);
}
