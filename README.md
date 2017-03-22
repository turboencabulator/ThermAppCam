# ThermAppCam Auto
This fork is designed to allow the Therm-App thermal imaging camera for use as a webcam-like device with minimal effort.

## New features
* Automatic non-uniformity calibration (keep lens cover on when starting software)
* Automatic dead pixel detection during the NUC process and correction
* Image range scaling
* YUV (V4L2_PIX_FMT_YUV420) instead of Grey (V4L2_PIX_FMT_GREY) output for better compatibility with security software (eg. motion or mjpgstreamer)
* Image is the right way round by default (instead of mirrored)
* Image can be flipped (arg = 1 for flip, = 0 for normal)

## Requirements
* V4L2Loopback
* libusb

## Todo
* Histogram Equalization instead of linear image range scaling
* Smarter dead pixel detection
* Colour LUT support
