# ThermAppCam Auto
This fork is designed to allow the Therm-App thermal imaging camera for use as a webcam-like device with minimal effort.

## New features
* Can use either the camera's factory calibration data (`-c` option) or automatic calibration
* Automatic non-uniformity calibration (keep lens cover on when starting software)
* Automatic bad pixel detection and correction during the NUC process
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

## Calibration
Your camera's factory calibration data is stored on ThermApp servers, not on the camera itself.  If you have used the official ThermApp Android app with your camera, it will connect and download that data on the first time you use it.  You can find these calibration files in your Android device's `ThermApp` directory.  Look for a subdirectory with the same name as your camera's serial number, which should contain files such as `0.bin`, `1.bin`, etc.  Be sure to keep backups of these files in case the server ever becomes unavailable!

This software may be able to use that factory calibration data in place of its automatic calibration.

To download the calibration files without using the official app, use the `get-calibration.py` script, found in the `calibration` directory.  You will need your camera's serial number and possibly the hardware and firmware versions; you can find these by viewing the first few lines of output from the `sudo thermapp` command above.  Then substitute `${SERIALNUMBER}` with your serial number in the command below, and run:
```
cd calibration
./get-calibration.py --serialNumber=${SERIALNUMBER}
```

This will create a subdirectory of the current directory with the same name as your camera's serial number (to match how the files are stored on Android), and then will download the calibration files to it.

The official ThermApp Android app sends other information to the server as part of the request, including your camera's hardware and firmware versions, and information about your Android device.  These may be necessary for the server to honor your request, see `./get-calibration.py --help` for a full list.

## Options
<dl>
<dt><code>-H</code></dt>
<dd>Flip the image horizontally.</dd>
<dt><code>-V</code></dt>
<dd>Flip the image vertically.</dd>
<dt><code>-c directory</code></dt>
<dd>Directory containing calibration data.  This directory should contain a subdirectory with the same name as your camera's serial number.</dd>
<dt><code>-d device</code></dt>
<dd>Send video to a particular video device.  The default device is <code>/dev/video0</code>.</dd>
<dt><code>-e[ratio]</code></dt>
<dd>Enhanced mode, also known as "night vision" mode.  Video frames are high-pass filtered.  The optional ratio is a parameter to this filter, and should be between 0.25 and 5.0 inclusive.  The default ratio is 1.25.  Low values produce a characteristic cold halo around warm objects.  High values produce an effect similar to edge detection.</dd>
<dt><code>-h</code></dt>
<dd>Show the help message and exit.</dd>
</dl>

## Troubleshooting
* Try a different cable.  Use a high-quality USB cable.
* Try plugging the camera into a different USB port.
* Make sure the camera is plugged into a USB 2.0-compatible port.  The camera will not work on USB 1.x ports since their max bulk transfer size is < 512 bytes.

## Todo
* Smarter bad pixel detection
* Automatically download the camera's factory calibration data
* Colour LUT support
