#!/bin/bash

echo "Installing HPS Audio Loader..."

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "ERROR: Run as root - sudo ./install.sh"
    exit 1
fi

# Check if binary exists
if [ ! -f "hps_audio_loader" ]; then
    echo "ERROR: Binary not found. Run 'make' first"
    exit 1
fi

# Install binary
cp hps_audio_loader /usr/bin/
chmod +x /usr/bin/hps_audio_loader

# Create songs directory
mkdir -p /media/sd/songs

# Create service file for auto-start
cat > /etc/systemd/system/hps-audio.service << 'EOF'
[Unit]
Description=HPS Audio Loader
After=network.target

[Service]
Type=simple
ExecStart=/usr/bin/hps_audio_loader
Restart=always
RestartSec=5
User=root

[Install]
WantedBy=multi-user.target
EOF

# Enable and start service
systemctl daemon-reload
systemctl enable hps-audio.service
systemctl start hps-audio.service

echo "Installation complete!"
echo "Service status:"
systemctl status hps-audio.service --no-pager -l

echo ""
echo "Commands:"
echo "  Start:   systemctl start hps-audio"
echo "  Stop:    systemctl stop hps-audio"
echo "  Status:  systemctl status hps-audio"
echo "  Manual:  sudo /usr/bin/hps_audio_loader"