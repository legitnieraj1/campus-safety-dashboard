#!/bin/bash
# run_gateway.sh — Forces ESP32 traffic through WiFi and runs the gateway
# Usage: ./run_gateway.sh

CLOUD_URL="https://safetyhub-relay.onrender.com"
ESP32_IP="192.168.4.1"
WIFI_IFACE="en0"

echo "=== SafetyHub Gateway Launcher ==="

# Check WiFi is on ESP32 network
WIFI_IP=$(ipconfig getifaddr $WIFI_IFACE 2>/dev/null)
if [[ "$WIFI_IP" != 192.168.4.* ]]; then
    echo "❌ WiFi is NOT connected to SafetyHub (current IP: $WIFI_IP)"
    echo "   Connect WiFi to 'SafetyHub' first, then run this script."
    exit 1
fi
echo "✅ WiFi connected to SafetyHub (IP: $WIFI_IP)"

# Force ESP32 subnet through WiFi — delete old route first, then add fresh
sudo route delete -net 192.168.4.0/24 2>/dev/null
sudo route add -net 192.168.4.0/24 -iface $WIFI_IFACE 2>/dev/null
echo "✅ Route forced: 192.168.4.x → $WIFI_IFACE"

# Test ESP32 reachability
echo "🔍 Testing ESP32 connectivity..."
if curl -s --max-time 3 "http://$ESP32_IP/api/data" > /dev/null; then
    echo "✅ ESP32 is reachable!"
else
    echo "⚠️  ESP32 not responding — check it's powered on"
fi

echo ""
echo "🚀 Starting gateway bridge → $CLOUD_URL"
echo ""

# Run countdown to let Render wake up
echo "⏳ Waiting 5s for Render to wake up..."
sleep 5

# Start the gateway
CLOUD_RELAY_URL=$CLOUD_URL python3 gateway/gateway.py
