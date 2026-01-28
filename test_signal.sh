#!/bin/bash

echo "Testing Ctrl+C signal handling..."

# Start the encoder in headless mode in background
cd /mnt/data/projects/poc/axia/converter/AACencoder-v1.0.12-SourceCode-fixed1/AACencoder-v1.0.12-SourceCode/build

# Create a minimal config for testing
cat > config/test_config.json << 'EOF'
{
    "webPort": 8080,
    "iceEnabled": false,
    "hlsEnabled": false,
    "metadataPort": 9090,
    "audioSource": "rtp",
    "rtpPort": 5004,
    "rtpAddress": "239.192.1.1"
}
EOF

echo "Starting encoder with test config..."
timeout 10s ./encoder --headless --config config/test_config.json &
ENCODER_PID=$!

echo "Encoder PID: $ENCODER_PID"
sleep 2

echo "Sending SIGINT (Ctrl+C) to encoder..."
kill -INT $ENCODER_PID

# Wait a bit and check if process is still running
sleep 3

if kill -0 $ENCODER_PID 2>/dev/null; then
    echo "❌ FAILED: Process still running after SIGINT"
    kill -KILL $ENCODER_PID
    exit 1
else
    echo "✅ SUCCESS: Process terminated gracefully after SIGINT"
    exit 0
fi
