#!/bin/sh
# Auto-start Audio Streamer for DE1-SoC
echo "=== Audio Streamer Auto-Start ==="
echo "Checking for hps_audio_loader..."

if [ -f /usr/bin/hps_audio_loader ]; then
    echo "Starting HPS Audio Streamer..."
    /usr/bin/hps_audio_loader &
    echo "Audio Streamer started in background (PID: $!)"
    echo "Ready for FPGA control via buttons/switches"
else
    echo "ERROR: /usr/bin/hps_audio_loader not found"
    echo "Please check installation"
fi

echo "=== Auto-Start Complete ==="
