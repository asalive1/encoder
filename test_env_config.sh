#!/bin/bash

echo "Testing environment variable config support..."

cd /mnt/data/projects/poc/axia/converter/AACencoder-v1.0.12-SourceCode-fixed1/AACencoder-v1.0.12-SourceCode/build

# Create a test config in a custom location
mkdir -p /tmp/custom_config
cat > /tmp/custom_config/my_encoder_config.json << 'EOF'
{
    "webPort": 9999,
    "iceEnabled": false,
    "hlsEnabled": false,
    "metadataPort": 9091,
    "audioSource": "rtp",
    "rtpPort": 5005,
    "rtpAddress": "239.192.1.2"
}
EOF

echo "Created custom config at /tmp/custom_config/my_encoder_config.json"

# Test 1: Default behavior (no env var)
echo "Test 1: Default config path"
timeout 3s ./encoder --headless 2>&1 | grep "Using config file:" || echo "Default test failed"

# Test 2: Environment variable override
echo "Test 2: Environment variable override"
export AAC_ENCODER_CONFIG="/tmp/custom_config/my_encoder_config.json"
timeout 3s ./encoder --headless 2>&1 | grep "Using config file: /tmp/custom_config/my_encoder_config.json"
if [ $? -eq 0 ]; then
    echo "✅ SUCCESS: Environment variable config path works"
else
    echo "❌ FAILED: Environment variable config path not working"
fi

# Test 3: CLI argument should override environment variable
echo "Test 3: CLI argument override"
timeout 3s ./encoder --headless --config config/test_config.json 2>&1 | grep "Using config file: config/test_config.json"
if [ $? -eq 0 ]; then
    echo "✅ SUCCESS: CLI argument overrides environment variable"
else
    echo "❌ FAILED: CLI argument override not working"
fi

# Cleanup
unset AAC_ENCODER_CONFIG
rm -rf /tmp/custom_config

echo "Environment variable config test completed"
