# capturedisp

Capture card display tool for CRT TVs on Raspberry Pi.

## Features
- USB capture card input (V4L2)
- 640x480 output for CRT
- Scanline-snapped vertical pixels
- Smooth or 1:1 horizontal stretch
- Crop and calibration controls
- Preset saving/loading
- No desktop environment required

## Dependencies
```bash
sudo apt install libsdl2-dev libv4l-dev v4l-utils
```

## Build
```bash
make
```

## Usage
```bash
capturedisp [options]
  -d, --device /dev/videoX   Capture device (default: /dev/video0)
  -p, --preset NAME          Load preset on start
  -l, --list                 List available presets
  -h, --help                 Show help
```

## Controls (while running)
- Arrow keys: Adjust crop position
- +/-: Adjust crop size
- S: Toggle smooth/1:1 horizontal stretch
- P: Save current settings as preset
- L: Load preset
- C: Enter calibration mode
- Q/Esc: Quit

## Presets
Stored in `~/.config/capturedisp/presets/`
