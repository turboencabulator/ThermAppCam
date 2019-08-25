#include "thermapp.h"

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

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
		return 0;
	}
	fprintf(stdout, "framesize %d\n", fs);
	fprintf(stdout, "linewidth %d\n", lw);
	if (framesize) *framesize = fs;
	if (linewidth) *linewidth = lw;

	return 1;
}

int main(int argc, char *argv[])
{
	ThermApp *therm = thermapp_open();
	if (!therm) {
		return -1;
	}

	if (thermapp_usb_connect(therm)
	 || thermapp_thread_create(therm)) {
		thermapp_close(therm);
		return -1;
	}

	int16_t frame[PIXELS_DATA_SIZE];
	int ret;

#ifndef FRAME_RAW
	int flipv = 0;
	if (argc >= 2) {
		flipv = *argv[1];
	}

	// get cal
	printf("Calibrating... cover the lens!\n");
	double pre_offset_cal = 0;
	double gain_cal = 1;
	double offset_cal = 0;
	long meancal = 0;
	int image_cal[PIXELS_DATA_SIZE];
	int deadpixel_map[PIXELS_DATA_SIZE] = { 0 };

	// Discard 1st frame, it usually has the header repeated twice
	// and the data shifted into the pad by a corresponding amount.
	ret = thermapp_getImage(therm, frame);
	if (ret) {
		thermapp_close(therm);
		return -1;
	}

	memset(image_cal, 0, sizeof image_cal);
	for (int i = 0; i < 50; i++) {
		printf("Captured calibration frame %d/50. Keep lens covered.\n", i+1);
		ret = thermapp_getImage(therm, frame);
		if (ret) {
			thermapp_close(therm);
			return -1;
		}

		for (int j = 0; j < PIXELS_DATA_SIZE; j++) {
			image_cal[j] += frame[j];
		}
	}

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
	printf("Calibration finished\n");
#endif

	struct v4l2_format vid_format;

	int fdwr = open(VIDEO_DEVICE, O_WRONLY);
	assert(fdwr >= 0);

	memset(&vid_format, 0, sizeof vid_format);
	vid_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	ret = ioctl(fdwr, VIDIOC_G_FMT, &vid_format);
	if (ret)
		perror("VIDIOC_G_FMT");

	vid_format.fmt.pix.width = FRAME_WIDTH;
	vid_format.fmt.pix.height = FRAME_HEIGHT;
	vid_format.fmt.pix.pixelformat = FRAME_FORMAT;
	vid_format.fmt.pix.field = V4L2_FIELD_NONE;
	vid_format.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;

	size_t framesize;
	size_t linewidth;
	if (!format_properties(vid_format.fmt.pix.pixelformat,
	                       vid_format.fmt.pix.width, vid_format.fmt.pix.height,
	                       &framesize,
	                       &linewidth)) {
		printf("unable to guess correct settings for format '%d'\n", FRAME_FORMAT);
	}
	vid_format.fmt.pix.sizeimage = framesize;
	vid_format.fmt.pix.bytesperline = linewidth;

	ret = ioctl(fdwr, VIDIOC_S_FMT, &vid_format);
	if (ret)
		perror("VIDIOC_S_FMT");

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
			if (flipv) {
				img[PIXELS_DATA_SIZE - ((i/384)+1)*384 + i%384] = x;
			} else {
				img[((i/384)+1)*384 - i%384 - 1] = x;
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

	close(fdwr);
	thermapp_close(therm);
	return 0;
}
