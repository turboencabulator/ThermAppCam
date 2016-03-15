#include "thermapp.h"
#include <stdio.h>

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

    thermapp_Close(therm);
    return 0;
}
