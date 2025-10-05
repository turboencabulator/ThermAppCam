// SPDX-FileCopyrightText: 2019-2025 Kyle Guinn <elyk03@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermapp.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <inttypes.h>
#include <math.h>
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

static size_t
v4l2_format_select(int fdwr,
                   uint32_t format,
                   size_t width,
                   size_t height)
{
	struct v4l2_format vid_format;

	memset(&vid_format, 0, sizeof vid_format);
	vid_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	if (ioctl(fdwr, VIDIOC_G_FMT, &vid_format)) {
		perror("VIDIOC_G_FMT");
		return 0;
	}

	vid_format.fmt.pix.width = width;
	vid_format.fmt.pix.height = height;
	vid_format.fmt.pix.pixelformat = format;
	vid_format.fmt.pix.field = V4L2_FIELD_NONE;
	vid_format.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;

	switch (vid_format.fmt.pix.pixelformat) {
	case V4L2_PIX_FMT_YUV420:
	case V4L2_PIX_FMT_YVU420:
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
		vid_format.fmt.pix.bytesperline = vid_format.fmt.pix.width; /* ??? */
		vid_format.fmt.pix.sizeimage = vid_format.fmt.pix.bytesperline * vid_format.fmt.pix.height;
		vid_format.fmt.pix.sizeimage += 2 * ((vid_format.fmt.pix.width + 1) / 2) * ((vid_format.fmt.pix.height + 1) / 2);
		break;
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_VYUY:
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_YVYU:
		vid_format.fmt.pix.bytesperline = 4 * ((vid_format.fmt.pix.width + 1) / 2);
		vid_format.fmt.pix.sizeimage = vid_format.fmt.pix.bytesperline * vid_format.fmt.pix.height;
		break;
	case V4L2_PIX_FMT_GREY:
		vid_format.fmt.pix.bytesperline = vid_format.fmt.pix.width;
		vid_format.fmt.pix.sizeimage = vid_format.fmt.pix.bytesperline * vid_format.fmt.pix.height;
		break;
	case V4L2_PIX_FMT_Y10:
	case V4L2_PIX_FMT_Y12:
	case V4L2_PIX_FMT_Y14:
	case V4L2_PIX_FMT_Y16:
	case V4L2_PIX_FMT_Y16_BE:
		vid_format.fmt.pix.bytesperline = 2 * vid_format.fmt.pix.width;
		vid_format.fmt.pix.sizeimage = vid_format.fmt.pix.bytesperline * vid_format.fmt.pix.height;
		break;
	default:
		fprintf(stderr, "unable to guess correct settings for format '%c%c%c%c'\n",
		        vid_format.fmt.pix.pixelformat       & 0xff,
		        vid_format.fmt.pix.pixelformat >>  8 & 0xff,
		        vid_format.fmt.pix.pixelformat >> 16 & 0xff,
		        vid_format.fmt.pix.pixelformat >> 24 & 0xff);
		return 0;
	}

	size_t ret = vid_format.fmt.pix.sizeimage;

	if (ioctl(fdwr, VIDIOC_S_FMT, &vid_format)) {
		perror("VIDIOC_S_FMT");
		return 0;
	}

	return ret;
}

int
main(int argc, char *argv[])
{
	int ret = EXIT_SUCCESS;
	struct thermapp_usb_dev *thermdev = NULL;
	struct thermapp_cal *thermcal = NULL;
	int fdwr = -1;
	uint8_t *img = NULL;
	size_t img_sz = 0;

	int fliph = 1;
	int flipv = 0;
	const char *caldir = NULL;
	const char *videodev = VIDEO_DEVICE;
	int opt;
	while ((opt = getopt(argc, argv, "HVc:d:h")) != -1) {
		switch (opt) {
		case 'H':
			fliph = 0;
			break;
		case 'V':
			flipv = 1;
			break;
		case 'c':
			caldir = optarg;
			break;
		case 'd':
			videodev = optarg;
			break;
		case 'h':
			printf("Usage: %s [options]\n", argv[0]);
			printf("  -H            Flip the image horizontally\n");
			printf("  -V            Flip the image vertically\n");
			printf("  -c dir        Path to the calibration directory\n");
			printf("  -d device     Write frames to selected device [default: " VIDEO_DEVICE "]\n");
			printf("  -h            Show this help message and exit\n");
			goto done;
		default:
			ret = EXIT_FAILURE;
			goto done;
		}
	}

	fdwr = open(videodev, O_WRONLY);
	if (fdwr < 0) {
		perror("open");
		ret = EXIT_FAILURE;
		goto done;
	}

	thermdev = thermapp_usb_open();
	if (!thermdev) {
		ret = EXIT_FAILURE;
		goto done;
	}

	union thermapp_frame frame;
	int ident_frame = 1;
#ifndef FRAME_RAW
	int autocal_frame = 50;
	int temp_settle_frame = 11;
	double temp_fpa = 0.0;
	double temp_therm = 0.0;
#endif
	thermapp_usb_start(thermdev);
	while (thermapp_usb_transfers_pending(thermdev)) {
		thermapp_usb_handle_events(thermdev);

		if (!thermapp_usb_frame_read(thermdev, &frame, sizeof frame)) {
			continue;
		}

		if (ident_frame) {
			ident_frame -= 1;

			thermcal = thermapp_cal_open(caldir, &frame.header);
			if (!thermcal) {
				ret = EXIT_FAILURE;
				break;
			}

			printf("Serial number: %" PRIu32 "\n", thermcal->serial_num);
			printf("Hardware number: %" PRIu16 "\n", thermcal->hardware_num);
			printf("Firmware number: %" PRIu16 "\n", thermcal->firmware_num);

			img_sz = v4l2_format_select(fdwr, FRAME_FORMAT, FRAME_WIDTH, FRAME_HEIGHT);
			if (!img_sz) {
				ret = EXIT_FAILURE;
				break;
			}

#ifndef FRAME_RAW
			img = malloc(img_sz);
			if (!img) {
				perror("malloc");
				ret = EXIT_FAILURE;
				break;
			}

			// Data in the U/V planes (when present) does not change.
			for (size_t i = FRAME_PIXELS; i < img_sz; ++i) {
				img[i] = 128;
			}

			printf("Calibrating... cover the lens!\n");
#endif

			// Discard 1st frame, it usually has the header repeated twice
			// and the data shifted into the pad by a corresponding amount.
			continue;
		}

		if (frame.header.data_w != FRAME_WIDTH
		 || frame.header.data_h != FRAME_HEIGHT) {
			continue;
		}

		const uint16_t *pixels = (const uint16_t *)&frame.bytes[frame.header.data_offset];
#ifndef FRAME_RAW
		if (autocal_frame) {
			autocal_frame -= 1;
			printf("\rCaptured calibration frame %d/50. Keep lens covered.", 50 - autocal_frame);
			fflush(stdout);

			for (size_t i = 0; i < FRAME_PIXELS; ++i) {
				thermcal->auto_offset[i] += pixels[i];
			}

			if (autocal_frame) {
				continue;
			}
			printf("\nCalibration finished\n");

			double meancal = 0;
			for (size_t i = 0; i < FRAME_PIXELS; ++i) {
				thermcal->auto_offset[i] /= 50.0f;
				meancal += thermcal->auto_offset[i];
			}
			meancal /= FRAME_PIXELS;
			// record the dead pixels
			for (size_t i = 0; i < FRAME_PIXELS; ++i) {
				if (fabs(thermcal->auto_offset[i] - meancal) > 250.0) {
					printf("Dead pixel ID: %d (%f vs %f)\n", i, thermcal->auto_offset[i], meancal);
				} else {
					thermcal->auto_live[i] = 1.0f;
				}
				thermcal->auto_offset[i] = -thermcal->auto_offset[i];
			}
		}

		double raw_temp = frame.header.temp_fpa_diode;
		double cur_temp_fpa = thermcal->coeffs_fpa_diode[1];
		cur_temp_fpa = fma(cur_temp_fpa, raw_temp, thermcal->coeffs_fpa_diode[0]);
		raw_temp = frame.header.temp_thermistor;
		double cur_temp_therm = thermcal->coeffs_thermistor[5];
		cur_temp_therm = fma(cur_temp_therm, raw_temp, thermcal->coeffs_thermistor[4]);
		cur_temp_therm = fma(cur_temp_therm, raw_temp, thermcal->coeffs_thermistor[3]);
		cur_temp_therm = fma(cur_temp_therm, raw_temp, thermcal->coeffs_thermistor[2]);
		cur_temp_therm = fma(cur_temp_therm, raw_temp, thermcal->coeffs_thermistor[1]);
		cur_temp_therm = fma(cur_temp_therm, raw_temp, thermcal->coeffs_thermistor[0]);
		if (temp_settle_frame) {
			temp_settle_frame -= 1;
			temp_fpa   = cur_temp_fpa;
			temp_therm = cur_temp_therm;
		} else {
			temp_fpa   = thermcal->alpha_fpa_diode  * temp_fpa   + (1.0 - thermcal->alpha_fpa_diode)  * cur_temp_fpa;
			temp_therm = thermcal->alpha_thermistor * temp_therm + (1.0 - thermcal->alpha_thermistor) * cur_temp_therm;
		}

		int uniform[FRAME_PIXELS];
		int frame_max;
		int frame_min;
		thermapp_img_nuc(thermcal, &frame, uniform, &frame_min, &frame_max);

		uint32_t frame_num = frame.header.frame_num_lo
		                   | frame.header.frame_num_hi << 16;
		printf("\rFrame #%" PRIu32 ":  FPA: %f C  Thermistor: %f C  Range: [%d:%d]", frame_num, cur_temp_fpa, cur_temp_therm, frame_min, frame_max);
		fflush(stdout);

		// second time through, this time actually scaling data
		for (size_t i = 0; i < FRAME_PIXELS; ++i) {
			int x = thermcal->nuc_live[i]
			      ? (((double)uniform[i] - frame_min)/(frame_max - frame_min)) * (235 - 16) + 16
			      : 16;
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
		write(fdwr, img, img_sz);
#else
		write(fdwr, pixels, FRAME_PIXELS * sizeof *pixels);
#endif
	}

done:
	if (img)
		free(img);
	if (thermcal)
		thermapp_cal_close(thermcal);
	if (thermdev)
		thermapp_usb_close(thermdev);
	if (fdwr >= 0)
		close(fdwr);
	return ret;
}
