# Raspberry KMBox

This is a lil kmbox implementation on the raspberry pi pico 1 and 2. It uses dual cores, 1 dedicated to host stack and one dedicated to the device stack. The rp2350 build has a full pio state machine running as a hardware accelerator. 

## Devices supported

I really only test this on an adafruit feather rp2040 usb host and and adafruit rp2350 hstx. It should run on anything though. 

## Hardware setup

The rp2040 doesn't need any changes. The rp2350 needs a usb host breakout connected with 3v3, ground, and then dm and dp connected to pins 10 and 11. Serial is through uart on 0 and 1, the board will run with or without it. 

## Is iT PaSsThRouGh

PID and VID are mirrored properly, some string descriptors are.

(Still a WIP)
