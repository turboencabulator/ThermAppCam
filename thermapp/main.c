// SPDX-FileCopyrightText: 2019-2025 Kyle Guinn <elyk03@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermapp.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_DEVICE "/dev/video0"
#undef FRAME_RAW

#ifndef FRAME_RAW
#define FRAME_FORMAT V4L2_PIX_FMT_YUV420
#else
#define FRAME_FORMAT V4L2_PIX_FMT_Y16
#endif

#define ROUND_UP_2(num)  (((num)+1)&~1)
#define ROUND_UP_4(num)  (((num)+3)&~3)
#define ROUND_UP_8(num)  (((num)+7)&~7)
#define ROUND_UP_16(num) (((num)+15)&~15)
#define ROUND_UP_32(num) (((num)+31)&~31)
#define ROUND_UP_64(num) (((num)+63)&~63)

static int
format_properties(const unsigned int format,
                  const unsigned int width,
                  const unsigned int height,
                  size_t *framesize,
                  size_t *linewidth)
{
	unsigned int lw, fs;
	switch (format) {
	case V4L2_PIX_FMT_YUV420:
	case V4L2_PIX_FMT_YVU420:
		lw = width; /* ??? */
		fs = ROUND_UP_4(width) * ROUND_UP_2(height);
		fs += 2 * ((ROUND_UP_8(width) / 2) * (ROUND_UP_2(height) / 2));
		break;
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_Y41P:
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_YVYU:
		lw = (ROUND_UP_2(width) * 2);
		fs = lw * height;
		break;
	case V4L2_PIX_FMT_Y10:
	case V4L2_PIX_FMT_Y12:
	case V4L2_PIX_FMT_Y16:
	case V4L2_PIX_FMT_Y16_BE:
		lw = 2 * width;
		fs = lw * height;
		break;
	default:
		return -1;
	}
	if (framesize) *framesize = fs;
	if (linewidth) *linewidth = lw;

	return 0;
}

int
main(int argc, char *argv[])
{
	int ret = EXIT_SUCCESS;
	struct thermapp_usb_dev *thermdev = NULL;
	int fdwr = -1;

	int fliph = 1;
	int flipv = 0;
	const char *videodev = VIDEO_DEVICE;
	int opt;
	while ((opt = getopt(argc, argv, "HVd:h")) != -1) {
		switch (opt) {
		case 'H':
			fliph = 0;
			break;
		case 'V':
			flipv = 1;
			break;
		case 'd':
			videodev = optarg;
			break;
		case 'h':
			printf("Usage: %s [options]\n", argv[0]);
			printf("  -H            Flip the image horizontally\n");
			printf("  -V            Flip the image vertically\n");
			printf("  -d device     Write frames to selected device [default: " VIDEO_DEVICE "]\n");
			printf("  -h            Show this help message and exit\n");
			goto done;
		default:
			ret = EXIT_FAILURE;
			goto done;
		}
	}

	thermdev = thermapp_usb_open();
	if (!thermdev) {
		ret = EXIT_FAILURE;
		goto done;
	}

	// Discard 1st frame, it usually has the header repeated twice
	// and the data shifted into the pad by a corresponding amount.
	struct thermapp_frame frame;
	if (thermapp_usb_connect(thermdev)
	 || thermapp_usb_thread_create(thermdev)
	 || thermapp_usb_frame_read(thermdev, &frame, sizeof frame)) {
		ret = EXIT_FAILURE;
		goto done;
	}

	uint32_t serial_num = frame.header.serial_num_lo
	                    | frame.header.serial_num_hi << 16;
	printf("Serial number: %" PRIu32 "\n", serial_num);
	printf("Hardware version: %" PRIu16 "\n", frame.header.hardware_ver);
	printf("Firmware version: %" PRIu16 "\n", frame.header.firmware_ver);

	// We don't know offset and quant value for temperature.
	// We use experimental value.
	printf("Temperature: %f\n", (frame.header.temperature - 14336) * 0.00652);

#ifndef FRAME_RAW
	// get cal
	double pre_offset_cal = 0;
	double gain_cal = 1;
	double offset_cal = 0;
	long meancal = 0;
	int image_cal[FRAME_PIXELS];
	int deadpixel_map[FRAME_PIXELS] = { 0 };

	memset(image_cal, 0, sizeof image_cal);
	printf("Calibrating... cover the lens!\n");
	for (int i = 0; i < 50; i++) {
		if (thermapp_usb_frame_read(thermdev, &frame, sizeof frame)) {
			goto done;
		}

		printf("\rCaptured calibration frame %d/50. Keep lens covered.", i+1);
		fflush(stdout);

		for (int j = 0; j < FRAME_PIXELS; j++) {
			image_cal[j] += frame.pixels[j];
		}
	}
	printf("\nCalibration finished\n");

	for (int i = 0; i < FRAME_PIXELS; i++) {
		image_cal[i] /= 50;
		meancal += image_cal[i];
	}
	meancal /= FRAME_PIXELS;
	// record the dead pixels
	for (int i = 0; i < FRAME_PIXELS; i++) {
		if ((image_cal[i] > meancal + 250) || (image_cal[i] < meancal - 250)) {
			printf("Dead pixel ID: %d (%d vs %li)\n", i, image_cal[i], meancal);
			deadpixel_map[i] = 1;
		}
	}
	// end of get cal
#endif

	struct v4l2_format vid_format;

	fdwr = open(videodev, O_WRONLY);
	if (fdwr < 0) {
		perror("open");
		ret = EXIT_FAILURE;
		goto done;
	}

	memset(&vid_format, 0, sizeof vid_format);
	vid_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	if (ioctl(fdwr, VIDIOC_G_FMT, &vid_format)) {
		perror("VIDIOC_G_FMT");
		ret = EXIT_FAILURE;
		goto done;
	}

	vid_format.fmt.pix.width = FRAME_WIDTH;
	vid_format.fmt.pix.height = FRAME_HEIGHT;
	vid_format.fmt.pix.pixelformat = FRAME_FORMAT;
	vid_format.fmt.pix.field = V4L2_FIELD_NONE;
	vid_format.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;

	size_t framesize;
	size_t linewidth;
	if (format_properties(vid_format.fmt.pix.pixelformat,
	                      vid_format.fmt.pix.width, vid_format.fmt.pix.height,
	                      &framesize,
	                      &linewidth)) {
		fprintf(stderr, "unable to guess correct settings for format '%d'\n", FRAME_FORMAT);
		ret = EXIT_FAILURE;
		goto done;
	}
	vid_format.fmt.pix.sizeimage = framesize;
	vid_format.fmt.pix.bytesperline = linewidth;

	if (ioctl(fdwr, VIDIOC_S_FMT, &vid_format)) {
		perror("VIDIOC_S_FMT");
		ret = EXIT_FAILURE;
		goto done;
	}

	while (thermapp_usb_frame_read(thermdev, &frame, sizeof frame) == 0) {
#ifndef FRAME_RAW
		uint8_t img[FRAME_PIXELS * 3 / 2];
		int i;
		int frameMax = ((frame.pixels[0] + pre_offset_cal - image_cal[0]) * gain_cal) + offset_cal;
		int frameMin = ((frame.pixels[0] + pre_offset_cal - image_cal[0]) * gain_cal) + offset_cal;
		for (i = 0; i < FRAME_PIXELS; i++) { // get the min and max values
			// only bother if the pixel isn't dead
			if (!deadpixel_map[i]) {
				int x = ((frame.pixels[i] + pre_offset_cal - image_cal[i]) * gain_cal) + offset_cal;
				if (x > frameMax) {
					frameMax = x;
				}
				if (x < frameMin) {
					frameMin = x;
				}
			}
		}
		// second time through, this time actually scaling data
		for (i = 0; i < FRAME_PIXELS; i++) {
			int x = ((frame.pixels[i] + pre_offset_cal - image_cal[i]) * gain_cal) + offset_cal;
			if (deadpixel_map[i]) {
				x = ((frame.pixels[i-1] + pre_offset_cal - image_cal[i-1]) * gain_cal) + offset_cal;
			}
			x = (((double)x - frameMin)/(frameMax - frameMin)) * (235 - 16) + 16;
			if (fliph && flipv) {
				img[FRAME_PIXELS - i] = x;
			} else if (fliph) {
				img[((i/FRAME_WIDTH)+1)*FRAME_WIDTH - i%FRAME_WIDTH - 1] = x;
			} else if (flipv) {
				img[FRAME_PIXELS - ((i/FRAME_WIDTH)+1)*FRAME_WIDTH + i%FRAME_WIDTH] = x;
			} else {
				img[i] = x;
			}
		}
		for (; i < sizeof img; i++) {
			img[i] = 128;
		}
		write(fdwr, img, sizeof img);
#else
		write(fdwr, frame.pixels, sizeof frame.pixels);
#endif
	}

done:
	if (fdwr >= 0)
		close(fdwr);
	if (thermdev)
		thermapp_usb_close(thermdev);
	return ret;
}
