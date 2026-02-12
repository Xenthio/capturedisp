#!/bin/bash
# capturedisp-ffplay - Fast video capture display using ffplay
# Crops NES region with pixel-perfect or 4:3 scaling

DEVICE="${1:-/dev/video0}"
USE_240P="${2:-1}"
MODE="${3:-smooth}"  # "smooth" (4:3 with h-blend) or "pixel" (pixel-perfect)

# NES capture region (measured from capture card output)
CROP_X=448
CROP_Y=83
CROP_W=1024
CROP_H=912

# NES native: 256x228, but original aspect is 4:3 (256x224 visible, stretched to 4:3)
# For 240p: screen is 720x480, we want centered output

# Set video mode
if [ "$USE_240P" = "1" ]; then
    echo "Switching to 240p (PAL60)..."
    tvservice -c "NTSC 4:3 P" 2>/dev/null
else
    echo "Switching to 480i (PAL60)..."
    tvservice -c "NTSC 4:3" 2>/dev/null
fi

# Apply PAL60 color encoding
sleep 0.5
python3 /home/pi/tweakvec.py --preset PAL60 2>/dev/null

# Output buffer is 720x480
SCREEN_W=720
SCREEN_H=480

if [ "$MODE" = "pixel" ]; then
    # Pixel-perfect: 256x228 scaled by 2 = 512x456, nearest neighbor
    SCALE_W=512
    SCALE_H=456
    SCALE_FLAGS="neighbor"
    echo "Mode: Pixel-perfect (512x456)"
else
    # 4:3 aspect: scale to fill height, smooth horizontal
    # Height: 480, maintain 4:3 = 640x480
    # Use bilinear for smooth horizontal scaling
    SCALE_W=640
    SCALE_H=480
    SCALE_FLAGS="bilinear"
    echo "Mode: 4:3 smooth (640x480)"
fi

# Calculate padding to center
PAD_X=$(( (SCREEN_W - SCALE_W) / 2 ))
PAD_Y=$(( (SCREEN_H - SCALE_H) / 2 ))

# Build filter chain
# 1. Crop the NES region
# 2. Scale to target size
# 3. Pad to center on 720x480
FILTER="crop=${CROP_W}:${CROP_H}:${CROP_X}:${CROP_Y}"
FILTER="${FILTER},scale=${SCALE_W}:${SCALE_H}:flags=${SCALE_FLAGS}"
FILTER="${FILTER},pad=${SCREEN_W}:${SCREEN_H}:${PAD_X}:${PAD_Y}:black"

echo "Crop: ${CROP_W}x${CROP_H} from (${CROP_X},${CROP_Y})"
echo "Scale: ${SCALE_W}x${SCALE_H}, pad to ${SCREEN_W}x${SCREEN_H}"
echo "Starting ffplay..."
echo "Press Q to quit"
echo ""
echo "To switch modes, restart with:"
echo "  capturedisp-ffplay /dev/video0 1 smooth  (4:3 smooth)"
echo "  capturedisp-ffplay /dev/video0 1 pixel   (pixel-perfect)"

# Run ffplay
exec ffplay -f v4l2 \
    -input_format yuyv422 \
    -video_size 1920x1080 \
    -framerate 60 \
    "$DEVICE" \
    -vf "$FILTER" \
    -an \
    -framedrop \
    -fs \
    -loglevel warning
