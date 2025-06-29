#!/bin/sh
echo "=== Complete FPGA Audio System Loader ==="

# Parar procesos anteriores
echo "Stopping previous processes..."
killall hps_audio_loader 2>/dev/null
sleep 2

# Cargar FPGA
echo "Programming FPGA with .rbf..."
if [ -f /audio_player.rbf ]; then
    cat /audio_player.rbf > /dev/fpga0
    sleep 3
    echo "FPGA programmed successfully"
else
    echo "ERROR: .rbf file not found"
    exit 1
fi

# Iniciar HPS Audio Streamer
echo "Starting HPS Audio Streamer..."
/usr/bin/hps_audio_loader &
sleep 2

echo "=== System Ready ==="
echo "- FPGA programmed with your .rbf"
echo "- HPS Audio Streamer active"
echo "- Ready for Eclipse FPGA software"
echo ""
echo "Next: Open Eclipse and run your Nios II project"
