# ThermAppCam Auto
This fork is designed to allow the Therm-App thermal imaging camera for use as a webcam-like device with minimal effort.

## New features
* Automatic non-uniformity calibration (keep lens cover on when starting software)
* Automatic dead pixel detection during the NUC process and correction
* Image range scaling
* YUV (V4L2_PIX_FMT_YUV420) instead of Grey (V4L2_PIX_FMT_GREY) output for better compatibility with security software (eg. motion or mjpgstreamer)
* Image is the right way round by default (instead of horizontally flipped)
* Image can be flipped horizontally or vertically (`-H` or `-V` options)

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

Loading the module will create a video device node such as `/dev/video0`.  The number may vary (particularly if you have other video cameras present), see v4l2loopback's documentation for details.

Plug in the camera.  Keep the lens covered as you start the software:
```
sudo thermapp [options]
```

The software will read 50 frames for its automatic calibration.  After that is complete, you may remove the lens cap and open the video device in your player of choice.

To quit, either press Ctrl+C or unplug the camera.

## Options
<dl>
<dt><code>-H</code></dt>
<dd>Flip the image horizontally.</dd>
<dt><code>-V</code></dt>
<dd>Flip the image vertically.</dd>
<dt><code>-d device</code></dt>
<dd>Send video to a particular video device.  The default device is <code>/dev/video0</code>.</dd>
<dt><code>-h</code></dt>
<dd>Show the help message and exit.</dd>
</dl>

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
