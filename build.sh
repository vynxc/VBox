#!/bin/bash

# PIO KMbox Build Script for RP2040

echo "Building PIO KMbox..."

# Clean and create build directory
rm -rf build
mkdir build
cd build

# Configure with CMake
echo "Configuring with CMake..."
cmake .. || {
    echo "CMake configuration failed!"
    exit 1
}

# Build the project
echo "Building project..."
make -j4 || {
    echo "Build failed!"
    exit 1
}

echo ""
echo "Build successful!"
echo "Generated files:"
ls -la *.uf2 *.elf

echo ""
echo "Attempting to flash with picotool..."

# Check if picotool is available
if command -v picotool &> /dev/null; then
    # Try to flash the firmware
    if picotool load PIOKMbox.uf2 --force; then
        echo "Firmware flashed successfully!"
        echo "Rebooting device..."
        picotool reboot
        echo "Device should now be running the new firmware."
    else
        echo "Picotool flash failed. Falling back to manual instructions:"
        echo "1. Hold BOOTSEL button while connecting USB"
        echo "2. Copy PIOKMbox.uf2 to the RPI-RP2 drive"
        echo "3. The device will reboot and run the firmware"
    fi
else
    echo "Picotool not found. Please install picotool for automatic flashing."
    echo "Manual flashing instructions:"
    echo "1. Hold BOOTSEL button while connecting USB"
    echo "2. Copy PIOKMbox.uf2 to the RPI-RP2 drive"
    echo "3. The device will reboot and run the firmware"
fi

echo ""
echo "UART output available at 115200 baud on pins:"
echo "- TX: GPIO 0"
echo "- RX: GPIO 1"