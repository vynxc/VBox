# TinyUSB Dual Host and Device HID Example for Adafruit RP2040

This project demonstrates a dual USB Host and Device HID implementation using TinyUSB on the Adafruit RP2040 microcontroller.

## Features

- **USB Device Mode**: Acts as a HID keyboard/mouse using the on-board USB controller
- **USB Host Mode**: Reads from connected HID devices (keyboard/mouse) using PIO-based USB host
- **Simultaneous Operation**: Both host and device functionality work concurrently
- **HID Forwarding**: Input from host devices can be forwarded to the device interface