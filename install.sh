#!/bin/bash
# Install script for capturedisp on Raspberry Pi

set -e

echo "Installing dependencies..."
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    libsdl2-dev \
    libv4l-dev \
    v4l-utils

echo "Building capturedisp..."
make clean
make

echo "Installing to /usr/local/bin..."
sudo make install

echo "Creating config directory..."
mkdir -p ~/.config/capturedisp/presets

echo ""
echo "Installation complete!"
echo "Run 'capturedisp' to start."
echo ""
echo "Tips:"
echo "  - Use 'v4l2-ctl --list-devices' to find your capture card"
echo "  - Run 'capturedisp -d /dev/videoX' to use a specific device"
echo "  - Press 'h' while running for help"
