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

			img_sz = v4l2_format_select(fdwr, FRAME_FORMAT, thermcal->img_w, thermcal->img_h);
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
			for (size_t i = thermcal->img_w * thermcal->img_h; i < img_sz; ++i) {
				img[i] = 128;
			}

			printf("Calibrating... cover the lens!\n");
#endif

			// Discard 1st frame, it usually has the header repeated twice
			// and the data shifted into the pad by a corresponding amount.
			continue;
		}

		if (frame.header.data_w != thermcal->img_w
		 || frame.header.data_h != thermcal->img_h) {
			continue;
		}

#ifndef FRAME_RAW
		if (autocal_frame) {
			autocal_frame -= 1;
			printf("\rCaptured calibration frame %d/50. Keep lens covered.", 50 - autocal_frame);
			fflush(stdout);

			const uint16_t *pixels = (const uint16_t *)&frame.bytes[frame.header.data_offset];
			size_t nuc_start = thermcal->ofs_y * thermcal->nuc_w + thermcal->ofs_x;
			size_t nuc_row_adj = thermcal->nuc_w - thermcal->img_w;
			float *nuc_good, *nuc_offset;

			nuc_offset = &thermcal->auto_offset[nuc_start];
			for (size_t y = thermcal->img_h; y; --y) {
				for (size_t x = thermcal->img_w; x; --x) {
					*nuc_offset++ += *pixels++;
				}
				nuc_offset += nuc_row_adj;
			}

			if (autocal_frame) {
				continue;
			}
			printf("\nCalibration finished\n");

			double meancal = 0.0;
			nuc_offset = &thermcal->auto_offset[nuc_start];
			for (size_t y = thermcal->img_h; y; --y) {
				for (size_t x = thermcal->img_w; x; --x) {
					*nuc_offset /= -50.0f;
					meancal += *nuc_offset++;
				}
				nuc_offset += nuc_row_adj;
			}
			meancal /= thermcal->img_w * thermcal->img_h;
			// record the bad pixels
			nuc_good   = &thermcal->auto_good[nuc_start];
			nuc_offset = &thermcal->auto_offset[nuc_start];
			for (size_t y = thermcal->img_h; y; --y) {
				for (size_t x = thermcal->img_w; x; --x) {
					if (fabs(*nuc_offset - meancal) > 250.0) {
						printf("Bad pixel (%zu,%zu) (%f vs %f)\n", thermcal->img_w - x, thermcal->img_h - y, *nuc_offset, meancal);
					} else {
						*nuc_good = 1.0f;
					}
					nuc_good   += 1;
					nuc_offset += 1;
				}
				nuc_good   += nuc_row_adj;
				nuc_offset += nuc_row_adj;
			}
			thermapp_cal_bpr_init(thermcal);
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

		float uniform[FRAME_PIXELS_MAX], frame_min, frame_max;
		thermapp_img_nuc(thermcal, &frame, uniform);
		thermapp_img_bpr(thermcal, uniform);
		thermapp_img_minmax(thermcal, uniform, &frame_min, &frame_max);

		uint32_t frame_num = frame.header.frame_num_lo
		                   | frame.header.frame_num_hi << 16;
		printf("\rFrame #%" PRIu32 ":  FPA: %f C  Thermistor: %f C  Range: [%f:%f]", frame_num, cur_temp_fpa, cur_temp_therm, frame_min, frame_max);
		fflush(stdout);

		// second time through, this time actually scaling data
		const float *in = uniform;
		uint8_t *out = img;
		int out_row_adj = 0;
		int out_col_adj = 1;
		if (fliph && flipv) {
			out += thermcal->img_w * thermcal->img_h - 1;
			out_col_adj = -1;
		} else if (fliph) {
			out += thermcal->img_w - 1;
			out_row_adj = 2 * thermcal->img_w;
			out_col_adj = -1;
		} else if (flipv) {
			out += thermcal->img_w * (thermcal->img_h - 1);
			out_row_adj = -(2 * thermcal->img_w);
		}
		for (size_t y = thermcal->img_h; y; --y) {
			for (size_t x = thermcal->img_w; x; --x) {
				float px = *in++;
				*out = ((px - frame_min)/(frame_max - frame_min)) * (235 - 16) + 16;
				out += out_col_adj;
			}
			out += out_row_adj;
		}
		write(fdwr, img, img_sz);
#else
		const uint16_t *pixels = (const uint16_t *)&frame.bytes[frame.header.data_offset];
		write(fdwr, pixels, frame.header.data_w * frame.header.data_h * sizeof *pixels);
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
