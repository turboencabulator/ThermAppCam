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
#define FRAME_WIDTH  384
#define FRAME_HEIGHT 288
//#define FRAME_FORMAT V4L2_PIX_FMT_GREY
#define FRAME_FORMAT V4L2_PIX_FMT_YUV420
#define ROUND_UP_2(num) (((num)+1)&~1)
#define ROUND_UP_4(num) (((num)+3)&~3)
#define ROUND_UP_8(num)  (((num)+7)&~7)
#define ROUND_UP_16(num) (((num)+15)&~15)
#define ROUND_UP_32(num) (((num)+31)&~31)
#define ROUND_UP_64(num) (((num)+63)&~63)

int format_properties(const unsigned int format,
                      const unsigned int width,
                      const unsigned int height,
                      size_t *linewidth,
                      size_t *framewidth)
{
	unsigned int lw, fw;
	switch (format) {
	case V4L2_PIX_FMT_YUV420:
	case V4L2_PIX_FMT_YVU420:
		lw = width; /* ??? */
		fw = ROUND_UP_4(width) * ROUND_UP_2(height);
		fw += 2 * ((ROUND_UP_8(width) / 2) * (ROUND_UP_2(height) / 2));
		break;
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_Y41P:
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_YVYU:
		lw = (ROUND_UP_2(width) * 2);
		fw = lw * height;
		break;
	case V4L2_PIX_FMT_Y10:
		fprintf(stdout,"S/W\n");
		lw = width;
		fw = width * height;
		break;
	default:
		return 0;
	}
	fprintf(stdout, "framewidth %d\n", fw);
	fprintf(stdout, "linewidth %d\n", lw);
	if (linewidth) *linewidth = lw;
	if (framewidth) *framewidth = fw;

	return 1;
}

int main(int argc, char *argv[])
{
	ThermApp *therm = thermapp_initUSB();
	if (!therm) {
		fputs("init error\n", stderr);
		return -1;
	}

	/// Debug -> check for thermapp
	if (thermapp_USB_checkForDevice(therm, VENDOR, PRODUCT) == -1) {
		fputs("USB_checkForDevice error\n", stderr);
		return -1;
	} else {
		puts("thermapp_FrameRequest_thread\n");
		//Run thread usb therm
		thermapp_FrameRequest_thread(therm);
	}

	short frame[PIXELS_DATA_SIZE];
	uint8_t img[165888];
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
	thermapp_GetImage(therm, frame);
	thermapp_GetImage(therm, frame);

	for (int i = 0; i < PIXELS_DATA_SIZE; i++) {
		image_cal[i] = frame[i];
	}

	for (int i = 0; i < 50; i++) {
		printf("Captured calibration frame %d/50. Keep lens covered.\n", i+1);
		thermapp_GetImage(therm, frame);

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

	struct v4l2_format vid_format;

	int fdwr = open(VIDEO_DEVICE, O_WRONLY);
	assert(fdwr >= 0);

	memset(&vid_format, 0, sizeof vid_format);
	vid_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	int ret_code = ioctl(fdwr, VIDIOC_G_FMT, &vid_format);
	if (ret_code < 0)
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
	                       &linewidth,
	                       &framesize)) {
		printf("unable to guess correct settings for format '%d'\n", FRAME_FORMAT);
	}
	vid_format.fmt.pix.sizeimage = framesize;
	vid_format.fmt.pix.bytesperline = linewidth;

	ret_code = ioctl(fdwr, VIDIOC_S_FMT, &vid_format);
	if (ret_code < 0)
		perror("VIDIOC_S_FMT");

	while (1) {
		thermapp_GetImage(therm, frame);
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
			x = (((double)x - frameMin)/(frameMax - frameMin))*255;
			if (flipv) {
				img[PIXELS_DATA_SIZE - ((i/384)+1)*384 + i%384] = x;
			} else {
				img[PIXELS_DATA_SIZE - 1 - (PIXELS_DATA_SIZE - ((i/384)+1)*384 + i%384)] = x;
			}
		}
		for (; i < 165888; i++) {
			img[i] = 128;
		}
		write(fdwr, img, 165888);
	}

	close(fdwr);
	thermapp_Close(therm);
	return 0;
}
