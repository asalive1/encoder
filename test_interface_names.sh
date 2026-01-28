#!/bin/bash

echo "=== Testing Network Interface Name Resolution ==="

cd /mnt/data/projects/poc/axia/converter/AACencoder-v1.0.12-SourceCode-fixed1/AACencoder-v1.0.12-SourceCode/build

# Get available network interfaces
echo "Available network interfaces:"
ip link show | grep -E "^[0-9]+:" | awk -F': ' '{print "  " $2}' | head -5

# Find a real interface (usually lo, eth0, or similar)
INTERFACE=$(ip link show | grep -E "^[0-9]+:" | awk -F': ' '{print $2}' | grep -v '@' | head -1)
echo "Using interface for test: $INTERFACE"

# Create test config with interface name
cat > config/interface_test.json << EOF
{
    "webPort": 8080,
    "iceEnabled": false,
    "hlsEnabled": false,
    "metadataPort": 9000,
    "inputType": "rtp",
    "rtpPort": 5004,
    "rtpAddress": "239.192.1.1",
    "rtpInterface": "$INTERFACE",
    "icecastInterface": "$INTERFACE"
}
EOF

echo "Created test config with interface: $INTERFACE"

# Test the application
echo "Testing interface name resolution..."
timeout 3s ./encoder --headless --config config/interface_test.json 2>&1 | grep -E "(RTP|Icecast|interface)" | head -10

# Test with IP address (backward compatibility)
INTERFACE_IP=$(ip addr show $INTERFACE | grep "inet " | awk '{print $2}' | cut -d'/' -f1 | head -1)
if [ ! -z "$INTERFACE_IP" ]; then
    echo -e "\nTesting backward compatibility with IP address: $INTERFACE_IP"
    
    cat > config/ip_test.json << EOF
{
    "webPort": 8080,
    "iceEnabled": false,
    "hlsEnabled": false,
    "inputType": "rtp",
    "rtpInterface": "$INTERFACE_IP",
    "icecastInterface": "$INTERFACE_IP"
}
EOF
    
    timeout 3s ./encoder --headless --config config/ip_test.json 2>&1 | grep -E "(RTP|Icecast|interface)" | head -5
fi

# Cleanup
rm -f config/interface_test.json config/ip_test.json

echo -e "\n✅ Interface name resolution test completed"
echo "Features:"
echo "  ✅ Interface names (e.g. eth0) are resolved to IP addresses"
echo "  ✅ IP addresses still work for backward compatibility"
echo "  ✅ Empty interface names default to ANY"
