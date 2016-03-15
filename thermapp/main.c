#include "thermapp.h"

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define VIDEO_DEVICE "/dev/video0"
#define FRAME_WIDTH  384
#define FRAME_HEIGHT 288
#define FRAME_FORMAT V4L2_PIX_FMT_GREY

#define ROUND_UP_2(num) (((num)+1)&~1)
#define ROUND_UP_4(num) (((num)+3)&~3)
#define ROUND_UP_8(num)  (((num)+7)&~7)
#define ROUND_UP_16(num) (((num)+15)&~15)
#define ROUND_UP_32(num) (((num)+31)&~31)
#define ROUND_UP_64(num) (((num)+63)&~63)

int format_properties(const unsigned int format,
                      const unsigned int width,
                      const unsigned int height,
                      size_t*linewidth,
                      size_t*framewidth) {
    unsigned int lw, fw;
    switch(format) {
    case V4L2_PIX_FMT_YUV420:
    case V4L2_PIX_FMT_YVU420:
        lw = width; /* ??? */
        fw = ROUND_UP_4 (width) * ROUND_UP_2 (height);
        fw += 2 * ((ROUND_UP_8 (width) / 2) * (ROUND_UP_2 (height) / 2));
        break;
    case V4L2_PIX_FMT_UYVY:
    case V4L2_PIX_FMT_Y41P:
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_YVYU:
        lw = (ROUND_UP_2 (width) * 2);
        fw = lw * height;
        break;
    case V4L2_PIX_FMT_GREY:
        fprintf(stdout,"S/W\n");
        lw = width;
        fw = width * height;
        break;
    default:
        return 0;
    }
    fprintf(stdout,"framewidth %d\n", fw);
    fprintf(stdout,"linewidth %d\n", lw);
    if(linewidth)*linewidth=lw;
    if(framewidth)*framewidth=fw;

    return 1;
}

int main(int argc, char *argv[]) {
    ThermApp *therm = thermapp_initUSB();
    if (therm == NULL) {
        fputs("init error\n", stderr);
        return -1;
    }

    /// Debug -> check for thermapp
    if (thermapp_USB_checkForDevice(therm, VENDOR, PRODUCT) == -1){
       fputs("USB_checkForDevice error\n", stderr);
       return -1;
    } else {
        puts("thermapp_FrameRequest_thread\n");
        //Run thread usb therm
        thermapp_FrameRequest_thread(therm);
    }

    struct v4l2_capability vid_caps;
    struct v4l2_format vid_format;

    const char *video_device = VIDEO_DEVICE;
    int fdwr = open(video_device, O_RDWR);
    assert(fdwr >= 0);

    int ret_code = ioctl(fdwr, VIDIOC_QUERYCAP, &vid_caps);
    assert(ret_code != -1);

    memset(&vid_format, 0, sizeof(vid_format));

    ret_code = ioctl(fdwr, VIDIOC_G_FMT, &vid_format);
#ifdef DEBUG
    print_format(&vid_format);
#endif
    size_t framesize;
    size_t linewidth;
    vid_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    vid_format.fmt.pix.width = FRAME_WIDTH;
    vid_format.fmt.pix.height = FRAME_HEIGHT;
    vid_format.fmt.pix.pixelformat = FRAME_FORMAT;
    vid_format.fmt.pix.sizeimage = framesize;
    vid_format.fmt.pix.field = V4L2_FIELD_NONE;
    vid_format.fmt.pix.bytesperline = linewidth;
    vid_format.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;

    ret_code = ioctl(fdwr, VIDIOC_S_FMT, &vid_format);

    //assert(ret_code != -1);

#ifdef DEBUG
    printf("frame: format=%d\tsize=%d\n", FRAME_FORMAT, framesize);
    print_format(&vid_format);
#endif

    if(!format_properties(vid_format.fmt.pix.pixelformat,
                          vid_format.fmt.pix.width, vid_format.fmt.pix.height,
                          &linewidth,
                          &framesize)) {
        printf("unable to guess correct settings for format '%d'\n", FRAME_FORMAT);
    }
    __u8 *buffer;
    __u8 *check_buffer;
    buffer=(__u8*)malloc(sizeof(__u8)*framesize);
    check_buffer=(__u8*)malloc(sizeof(__u8)*framesize);

    short frame[PIXELS_DATA_SIZE];
    while (1) {
      if (thermapp_GetImage(therm, frame)) {
        int pix_lim, i;

        unsigned char img[PIXELS_DATA_SIZE];

        for (i = 0; i < PIXELS_DATA_SIZE; i++) {
            pix_lim = frame[i];

            if (pix_lim > 255) {
                pix_lim = 255;
            } else if (pix_lim < 0) {
                pix_lim = 0;
            }

            img[i] = (unsigned char) pix_lim;
        }

        memset(buffer, 0, framesize);
        memset(check_buffer, 0, framesize);
        //buffer = img;

        for (i = 0; i < framesize; ++i) {
            buffer[i] = img[i];
            check_buffer[i] = 0;
        }

        write(fdwr, buffer, framesize);
      }
      sleep(100);
    }

    close(fdwr);
    thermapp_Close(therm);
    return 0;
}
