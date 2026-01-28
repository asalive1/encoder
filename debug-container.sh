#!/bin/bash

echo "=== Container Debug Script ==="

cd /mnt/data/projects/poc/axia/converter/AACencoder-v1.0.12-SourceCode-fixed1/AACencoder-v1.0.12-SourceCode

# Ensure local directories exist with correct permissions
mkdir -p logs config HLS
chmod 755 logs config HLS

# Create minimal config
cat > config/debug-config.json << 'EOF'
{
    "webPort": 8080,
    "iceEnabled": false,
    "hlsEnabled": false,
    "logDir": "/app/logs"
}
EOF

echo "1. Building debug container..."
docker build -f Dockerfile.debug -t aac-encoder-debug . --quiet

echo "2. Testing container permissions..."
docker run --rm aac-encoder-debug ls -la /app/

echo "3. Running container with volume mounts..."
docker run -d \
    --name aac-debug \
    -p 8080:8080 \
    -v $(pwd)/config:/app/config \
    -v $(pwd)/logs:/app/logs \
    -e AAC_ENCODER_CONFIG=/app/config/debug-config.json \
    aac-encoder-debug

echo "4. Waiting for startup..."
sleep 3

echo "5. Checking container logs..."
docker logs aac-debug

echo "6. Checking log files..."
ls -la logs/

echo "7. Testing web UI..."
curl -s http://localhost:8080/status || echo "Web UI not responding"

echo "8. Cleanup..."
docker stop aac-debug >/dev/null 2>&1
docker rm aac-debug >/dev/null 2>&1

echo "Debug complete."
