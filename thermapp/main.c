#include "thermapp.h"

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#define VIDEO_DEVICE "/dev/video0"
#undef FRAME_RAW

#ifndef FRAME_RAW
#define FRAME_FORMAT V4L2_PIX_FMT_YUV420
#else
#define FRAME_FORMAT V4L2_PIX_FMT_Y16
#endif

#define ROUND_UP_2(num) (((num)+1)&~1)
#define ROUND_UP_4(num) (((num)+3)&~3)
#define ROUND_UP_8(num)  (((num)+7)&~7)
#define ROUND_UP_16(num) (((num)+15)&~15)
#define ROUND_UP_32(num) (((num)+31)&~31)
#define ROUND_UP_64(num) (((num)+63)&~63)

int format_properties(const unsigned int format,
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

int main(int argc, char *argv[])
{
	int16_t frame[PIXELS_DATA_SIZE];
	int ret = EXIT_SUCCESS;

	int fliph = 1;
	int flipv = 0;
	int opt;
	while ((opt = getopt(argc, argv, "HVh")) != -1) {
		switch (opt) {
		case 'H':
			fliph = 0;
			break;
		case 'V':
			flipv = 1;
			break;
		case 'h':
			printf("Usage: %s [options]\n", argv[0]);
			printf("  -H            Flip the image horizontally\n");
			printf("  -V            Flip the image vertically\n");
			printf("  -h            Show this help message and exit\n");
			goto done1;
		default:
			ret = EXIT_FAILURE;
			goto done1;
		}
	}

	ThermApp *therm = thermapp_open();
	if (!therm) {
		ret = EXIT_FAILURE;
		goto done1;
	}

	// Discard 1st frame, it usually has the header repeated twice
	// and the data shifted into the pad by a corresponding amount.
	if (thermapp_usb_connect(therm)
	 || thermapp_thread_create(therm)
	 || thermapp_getImage(therm, frame)) {
		ret = EXIT_FAILURE;
		goto done2;
	}

	printf("Serial number: %d\n", thermapp_getSerialNumber(therm));
	printf("Hardware version: %d\n", thermapp_getHardwareVersion(therm));
	printf("Firmware version: %d\n", thermapp_getFirmwareVersion(therm));

#ifndef FRAME_RAW
	// get cal
	double pre_offset_cal = 0;
	double gain_cal = 1;
	double offset_cal = 0;
	long meancal = 0;
	int image_cal[PIXELS_DATA_SIZE];
	int deadpixel_map[PIXELS_DATA_SIZE] = { 0 };

	memset(image_cal, 0, sizeof image_cal);
	printf("Calibrating... cover the lens!\n");
	for (int i = 0; i < 50; i++) {
		if (thermapp_getImage(therm, frame)) {
			goto done2;
		}

		printf("\rCaptured calibration frame %d/50. Keep lens covered.", i+1);
		fflush(stdout);

		for (int j = 0; j < PIXELS_DATA_SIZE; j++) {
			image_cal[j] += frame[j];
		}
	}
	printf("\nCalibration finished\n");

	for (int i = 0; i < PIXELS_DATA_SIZE; i++) {
		image_cal[i] /= 50;
		meancal += image_cal[i];
	}
	meancal /= PIXELS_DATA_SIZE;
	// record the dead pixels
	for (int i = 0; i < PIXELS_DATA_SIZE; i++) {
		if ((image_cal[i] > meancal + 250) || (image_cal[i] < meancal - 250)) {
			printf("Dead pixel ID: %d (%d vs %li)\n", i, image_cal[i], meancal);
			deadpixel_map[i] = 1;
		}
	}
	// end of get cal
#endif

	struct v4l2_format vid_format;

	int fdwr = open(VIDEO_DEVICE, O_WRONLY);
	if (fdwr < 0) {
		perror("open");
		ret = EXIT_FAILURE;
		goto done2;
	}

	memset(&vid_format, 0, sizeof vid_format);
	vid_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	if (ioctl(fdwr, VIDIOC_G_FMT, &vid_format)) {
		perror("VIDIOC_G_FMT");
		ret = EXIT_FAILURE;
		goto done3;
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
		goto done3;
	}
	vid_format.fmt.pix.sizeimage = framesize;
	vid_format.fmt.pix.bytesperline = linewidth;

	if (ioctl(fdwr, VIDIOC_S_FMT, &vid_format)) {
		perror("VIDIOC_S_FMT");
		ret = EXIT_FAILURE;
		goto done3;
	}

	while (thermapp_getImage(therm, frame) == 0) {
#ifndef FRAME_RAW
		uint8_t img[PIXELS_DATA_SIZE * 3 / 2];
		int i;
		int frameMax = ((frame[0] + pre_offset_cal - image_cal[0]) * gain_cal) + offset_cal;
		int frameMin = ((frame[0] + pre_offset_cal - image_cal[0]) * gain_cal) + offset_cal;
		for (i = 0; i < PIXELS_DATA_SIZE; i++) { // get the min and max values
			// only bother if the pixel isn't dead
			if (!deadpixel_map[i]) {
				int x = ((frame[i] + pre_offset_cal - image_cal[i]) * gain_cal) + offset_cal;
				if (x > frameMax) {
					frameMax = x;
				}
				if (x < frameMin) {
					frameMin = x;
				}
			}
		}
		// second time through, this time actually scaling data
		for (i = 0; i < PIXELS_DATA_SIZE; i++) {
			int x = ((frame[i] + pre_offset_cal - image_cal[i]) * gain_cal) + offset_cal;
			if (deadpixel_map[i]) {
				x = ((frame[i-1] + pre_offset_cal - image_cal[i-1]) * gain_cal) + offset_cal;
			}
			x = (((double)x - frameMin)/(frameMax - frameMin)) * (235 - 16) + 16;
			if (fliph && flipv) {
				img[PIXELS_DATA_SIZE - i] = x;
			} else if (fliph) {
				img[((i/FRAME_WIDTH)+1)*FRAME_WIDTH - i%FRAME_WIDTH - 1] = x;
			} else if (flipv) {
				img[PIXELS_DATA_SIZE - ((i/FRAME_WIDTH)+1)*FRAME_WIDTH + i%FRAME_WIDTH] = x;
			} else {
				img[i] = x;
			}
		}
		for (; i < sizeof img; i++) {
			img[i] = 128;
		}
		write(fdwr, img, sizeof img);
#else
		write(fdwr, frame, sizeof frame);
#endif
	}

done3:
	close(fdwr);
done2:
	thermapp_close(therm);
done1:
	return ret;
}
