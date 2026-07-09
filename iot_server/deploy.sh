#!/bin/bash
# deploy.sh — Deploy ESP32 Bridge Server to /www/wwwroot/esp32-bridge
# Usage:  scp -r server/* root@8.148.208.229:/www/wwwroot/esp32-bridge/
#         ssh root@8.148.208.229 "cd /www/wwwroot/esp32-bridge && bash deploy.sh"

set -e

BASE="/www/wwwroot/esp32-bridge"

echo "=== Installing dependencies ==="
pip3 install -r requirements.txt

echo "=== Creating systemd service ==="
cat > /etc/systemd/system/esp32-bridge.service << 'SERV'
[Unit]
Description=ESP32 WebSocket Bridge
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/www/wwwroot/esp32-bridge
ExecStart=/usr/bin/python3 /www/wwwroot/esp32-bridge/main.py
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
SERV

systemctl daemon-reload
systemctl enable esp32-bridge
systemctl restart esp32-bridge

echo "=== Done ==="
systemctl status esp32-bridge --no-pager
