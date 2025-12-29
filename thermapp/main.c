// SPDX-FileCopyrightText: 2019-2025 Kyle Guinn <elyk03@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermapp.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLOCK_SOURCE CLOCK_MONOTONIC

#define VIDEO_DEVICE "/dev/video0"
#define FRAME_FORMAT V4L2_PIX_FMT_YUV420

static int
v4l2_open(const char *videodev)
{
	int fd = open(videodev, O_WRONLY);
	if (fd < 0) {
		perror("open");
		return -1;
	}

	struct v4l2_capability cap;
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		perror("VIDIOC_QUERYCAP");
		return -1;
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)) {
		fprintf(stderr, "%s is not a video output device\n", videodev);
		return -1;
	}

	if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
		fprintf(stderr, "%s does not support write i/o\n", videodev);
		return -1;
	}

	return fd;
}

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

static float
timespec_delta(struct timespec end, struct timespec start)
{
	struct timespec delta;
	delta.tv_nsec = end.tv_nsec - start.tv_nsec;
	delta.tv_sec  = end.tv_sec  - start.tv_sec;
	return (float)delta.tv_sec + (float)delta.tv_nsec / 1e9f;
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
	enum thermapp_video_mode video_mode = VIDEO_MODE_THERMOGRAPHY;
	float enhanced_ratio = 1.25f;
	int opt;
	while ((opt = getopt(argc, argv, "HVc:d:e::h")) != -1) {
		switch (opt) {
		case 'H':
			fliph = !fliph;
			break;
		case 'V':
			flipv = !flipv;
			break;
		case 'c':
			caldir = optarg;
			break;
		case 'd':
			videodev = optarg;
			break;
		case 'e':
			video_mode = VIDEO_MODE_ENHANCED;
			if (optarg) {
				enhanced_ratio = strtof(optarg, NULL);
				if (enhanced_ratio < 0.25f) {
					enhanced_ratio = 0.25f;
				} else if (enhanced_ratio > 5.0f) {
					enhanced_ratio = 5.0f;
				}
			}
			break;
		case 'h':
			printf("Usage: %s [options]\n", argv[0]);
			printf("  -H            Flip the image horizontally\n");
			printf("  -V            Flip the image vertically\n");
			printf("  -c dir        Path to the calibration directory\n");
			printf("  -d device     Write frames to selected device [default: " VIDEO_DEVICE "]\n");
			printf("  -e[ratio]     Enhanced (\"night vision\") video mode\n");
			printf("                Enhanced ratio: 0.25 to 5.0 [default: 1.25]\n");
			printf("  -h            Show this help message and exit\n");
			goto done;
		default:
			ret = EXIT_FAILURE;
			goto done;
		}
	}

	fdwr = v4l2_open(videodev);
	if (fdwr < 0) {
		ret = EXIT_FAILURE;
		goto done;
	}

	thermdev = thermapp_usb_open();
	if (!thermdev) {
		ret = EXIT_FAILURE;
		goto done;
	}

	int resume_req = 2;
	int ident_frame = 1;
	int autocal_frame = 0;
	int temp_settle_frame = 0;
	int transient_steps = 0;
	double temp_fpa = 0.0;
	double temp_therm = 0.0;
	double old_temp_delta = 0.0;
	double old_deriv_temp_delta = 0.0;
	struct timespec transient_start = { 0 };
	struct timespec transient_step_start = { 0 };
	uint16_t vgsk = thermapp_initial_cfg.VoutC;
	uint8_t palette_index[UINT16_MAX+1];
	uint8_t palette[UINT8_MAX+1];

	memset(palette_index, 0, sizeof palette_index);
	for (size_t i = 0; i < UINT8_MAX+1; ++i) {
		if (i < 16) {
			palette[i] = 16;
		} else if (i > 235) {
			palette[i] = 235;
		} else {
			palette[i] = i;
		}
	}

	thermapp_usb_start(thermdev);
	while (thermapp_usb_transfers_pending(thermdev)) {
		thermapp_usb_handle_events(thermdev);

		union thermapp_frame frame;
		if (!thermapp_usb_frame_read(thermdev, &frame, sizeof frame)) {
			if (resume_req) {
				resume_req -= 1;

				uint16_t mode = 2;
				thermapp_usb_cfg_write(thermdev, &mode, offsetof(union thermapp_cfg, modes), sizeof mode);
				thermapp_usb_cfg_write(thermdev, NULL, 0, 0);
			}
			continue;
		}

		if (ident_frame) {
			ident_frame -= 1;

			// Suspend, ideally until there is demand for the video.
			// The camera holds the last 512-byte packet of the current frame until
			// the following resume.  Note that this matches the start-up behavior
			// where the first frame is preceded with 512 bytes of 0xff i.e. the
			// last packet of the (nonexistent) frame before the first one.
			uint16_t mode = 1;
			thermapp_usb_cfg_write(thermdev, &mode, offsetof(union thermapp_cfg, modes), sizeof mode);
			thermapp_usb_cfg_write(thermdev, NULL, 0, 0);

			thermcal = thermapp_cal_open(caldir, &frame.header);
			if (!thermcal) {
				ret = EXIT_FAILURE;
				break;
			}

			printf("Serial number: %" PRIu32 "\n", thermcal->serial_num);
			printf("Hardware version: %" PRIu16 "\n", thermcal->hardware_ver);
			printf("Firmware version: %" PRIu16 "\n", thermcal->firmware_ver);

			img_sz = v4l2_format_select(fdwr, FRAME_FORMAT, thermcal->img_w, thermcal->img_h);
			if (!img_sz) {
				ret = EXIT_FAILURE;
				break;
			}

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

			// Restart temp sensor measurements.
			clock_gettime(CLOCK_SOURCE, &transient_start);
			transient_step_start = transient_start;
			transient_steps = thermcal->transient_steps_max;
			temp_settle_frame = 11;
			old_temp_delta = NAN;
			old_deriv_temp_delta = NAN;

			// Use factory cal and/or restart autocal.
			if (thermapp_cal_present(thermcal)) {
				thermapp_cal_bpr_init(thermcal);
			} else {
				autocal_frame = 50;
				printf("Calibrating... cover the lens!\n");
			}

			// TODO: Cannot detect video demand.  Resume after reading calibration.
			resume_req = 3;

			// Discard 1st frame, it usually has the header repeated twice
			// and the data shifted into the pad by a corresponding amount.
			continue;
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
			temp_fpa   = thermcal->beta_fpa_diode  * temp_fpa   + (1.0 - thermcal->beta_fpa_diode)  * cur_temp_fpa;
			temp_therm = thermcal->beta_thermistor * temp_therm + (1.0 - thermcal->beta_thermistor) * cur_temp_therm;
		}

		double temp_delta = temp_therm - temp_fpa;
		if (transient_steps) {
			struct timespec now;
			clock_gettime(CLOCK_SOURCE, &now);
			if (timespec_delta(now, transient_step_start) > thermcal->transient_step_time) {
				transient_step_start = now;
				if (timespec_delta(now, transient_start) >= thermcal->transient_oper_time
				 || fabs(temp_delta) > thermcal->temp_delta_max) {
					transient_steps = 0;
				} else if (!isnan(old_temp_delta)) {
					double deriv_temp_delta = temp_delta - old_temp_delta;
					if (!isnan(old_deriv_temp_delta)) {
						deriv_temp_delta = thermcal->beta_deriv_temp_delta * old_deriv_temp_delta + (1.0 - thermcal->beta_deriv_temp_delta) * deriv_temp_delta;
						if (fabs(deriv_temp_delta) < thermcal->deriv_temp_delta_min) {
							transient_steps -= 1;
						} else {
							transient_steps = thermcal->transient_steps_max;
						}
					}
					old_deriv_temp_delta = deriv_temp_delta;
				}
				old_temp_delta = temp_delta;
			}
		}

		// All autocal and image processing below expects the image size to match that of the ident frame.
		if (frame.header.data_w != thermcal->img_w
		 || frame.header.data_h != thermcal->img_h) {
			continue;
		}

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

			// Skip image processing until autocal data is ready to use.
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

		if (thermapp_cal_select(thermcal, thermdev, video_mode, temp_therm)
		// XXX: Don't update vgsk/VoutC on every frame, else it will go bistable
		// since we begin seeing the effects of the vgsk/VoutC write immediately.
		// Instead wait the about-two-frame delay for the previous write to
		// complete (i.e. until the incoming header matches the outgoing header).
		//
		// Example:  We receive frame 1, calculate a delta to apply to vgsk/VoutC
		// from the pixel data (which steps toward the target vgsk/VoutC value),
		// and send that new value.  The delta begins to take effect while frame 2
		// is being captured; later scanlines have increasingly larger or smaller
		// values to match the gain change.  However the header for frame 2 does
		// not contain the updated vgsk/VoutC value, it still reports the original
		// value.  Next we receive frame 2, calculate a new delta from the original
		// vgsk/VoutC (smaller than what was calculated from frame 1 because of the
		// gain taking effect, which is a retreat from the target value), and send
		// it.  The process repeats, with frame 3 reporting the vgsk/VoutC computed
		// from frame 1 but with its pixel values retreating because of frame 2,
		// resulting in a larger step than desired toward the target value.
		 || vgsk == frame.header.VoutC) {
			// If for some reason switching to the autocal set, don't adjust
			// vgsk/VoutC since that cal is only valid at a particular value.
			if (thermcal->cur_set < CAL_SETS) {
				vgsk = thermapp_img_vgsk(thermcal, &frame);
				thermapp_usb_cfg_write(thermdev, &vgsk, offsetof(union thermapp_cfg, VoutC), sizeof vgsk);
			} else {
				vgsk = thermapp_initial_cfg.VoutC;
			}
		}

		// XXX: Sometimes vgsk/VoutC in the response never updates to match the
		// most recent request.  Unclear if that request is queued, or if it took
		// effect and the response header was never updated.  May be timing related.
		// Send the request on every received frame until it updates.
		// See also resume_req, may need a 2nd/3rd write to resume after suspend.
		thermapp_usb_cfg_write(thermdev, NULL, 0, 0);

		float uniform[FRAME_PIXELS_MAX], frame_min, frame_max;
		uint16_t quantized[FRAME_PIXELS_MAX];
		thermapp_img_nuc(thermcal, &frame, uniform, !!transient_steps, temp_delta);
		thermapp_img_bpr(thermcal, uniform);
		thermapp_img_minmax(thermcal, uniform, &frame_min, &frame_max);
		thermapp_img_quantize(thermcal, uniform, quantized);
		if (video_mode == VIDEO_MODE_ENHANCED) {
			thermapp_img_hpf(thermcal, quantized, enhanced_ratio);
		}
		thermapp_img_lut(thermcal, quantized, palette_index, 0.0f, 0.0f);

		uint32_t frame_num = frame.header.frame_num_lo
		                   | frame.header.frame_num_hi << 16;
		printf("\rFrame #%" PRIu32 ":  FPA: %f C  Thermistor: %f C  Range: [%f:%f]", frame_num, cur_temp_fpa, cur_temp_therm, frame_min, frame_max);
		fflush(stdout);

		const uint16_t *in = quantized;
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
				*out = palette[palette_index[*in++]];
				out += out_col_adj;
			}
			out += out_row_adj;
		}
		write(fdwr, img, img_sz);
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
