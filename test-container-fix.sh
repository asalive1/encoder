#!/bin/bash

echo "=== Testing Container Fix ==="

cd /mnt/data/projects/poc/axia/converter/AACencoder-v1.0.12-SourceCode-fixed1/AACencoder-v1.0.12-SourceCode

# Clean up any existing containers
docker stop aac-test 2>/dev/null || true
docker rm aac-test 2>/dev/null || true

# Ensure local directories exist
mkdir -p logs config HLS
chmod 755 logs config HLS

# Create minimal config
cat > config/test-config.json << 'EOF'
{
    "webPort": 8080,
    "iceEnabled": false,
    "hlsEnabled": false,
    "logDir": "/app/logs"
}
EOF

echo "1. Building container..."
docker build -t aac-encoder-test . --quiet

echo "2. Running container..."
docker run -d \
    --name aac-test \
    -p 8080:8080 \
    -v $(pwd)/config:/app/config \
    -v $(pwd)/logs:/app/logs \
    -e AAC_ENCODER_CONFIG=/app/config/test-config.json \
    aac-encoder-test

echo "3. Waiting for startup (10 seconds)..."
sleep 10

echo "4. Container logs:"
docker logs aac-test | head -10

echo "5. Log files created:"
ls -la logs/ 2>/dev/null || echo "No log files found"

echo "6. Testing web UI:"
if curl -s http://localhost:8080/status >/dev/null 2>&1; then
    echo "✅ Web UI is responding"
else
    echo "❌ Web UI not responding"
fi

echo "7. Container status:"
docker ps --filter name=aac-test --format "table {{.Names}}\t{{.Status}}"

echo "8. Cleanup..."
docker stop aac-test >/dev/null 2>&1
docker rm aac-test >/dev/null 2>&1

echo "Test complete."
