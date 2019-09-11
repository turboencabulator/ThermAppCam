# ThermAppCam Auto
This fork is designed to allow the Therm-App thermal imaging camera for use as a webcam-like device with minimal effort.

## New features
* Automatic non-uniformity calibration (keep lens cover on when starting software)
* Automatic dead pixel detection during the NUC process and correction
* Image range scaling
* YUV (V4L2_PIX_FMT_YUV420) instead of Grey (V4L2_PIX_FMT_GREY) output for better compatibility with security software (eg. motion or mjpgstreamer)
* Image is the right way round by default (instead of horizontally flipped)
* Image can be vertically flipped (arg = 1 for vflip, = 0 for normal)

## Dependencies
* [v4l2loopback](https://github.com/umlaeute/v4l2loopback)
* [libusb](https://libusb.info/) >= 1.0

## Installation
```
cd thermapp
make
sudo make install
```

## Usage
First make sure the v4l2loopback kernel module is loaded:
```
sudo modprobe v4l2loopback
```

Plug in the camera.  Keep the lens covered as you start the software:
```
sudo thermapp
```

The software will read 50 frames for its automatic calibration.  After that is complete, you may remove the lens cap and open `/dev/video0` in your video player of choice.

To quit, either press Ctrl+C or unplug the camera.

## Troubleshooting
* Try a different cable.  Use a high-quality USB cable.
* Try plugging the camera into a different USB port.
* Make sure the camera is plugged into a USB 2.0-compatible port.  The camera will not work on USB 1.x ports since their max bulk transfer size is < 512 bytes.
* Edit `TRANSFER_SIZE` in thermapp.h and then re-install.  Try reducing it to 512.  Reducing the transfer size will result in higher CPU load.

## Todo
* Histogram Equalization instead of linear image range scaling
* Smarter dead pixel detection
* Download and use the camera's calibration data
* Colour LUT support
