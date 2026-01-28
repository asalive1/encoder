#!/bin/bash
# Test script to verify RTP address filtering fix

echo "=== RTP Address Fix Verification ==="
echo ""

# Check if the fix is in place
echo "1. Checking if fix is applied in source code..."
if grep -q "inet_addr(cfg.rtpAddress.c_str())" src/main.cpp; then
    echo "   ✓ Fix is present: Binding to specific multicast address"
else
    echo "   ✗ Fix NOT found: Still using INADDR_ANY"
    exit 1
fi

# Check if old bug is removed
if grep -q "htonl(INADDR_ANY)" src/main.cpp | grep -q "local.sin_addr.s_addr"; then
    echo "   ✗ WARNING: Old INADDR_ANY binding still present"
else
    echo "   ✓ Old INADDR_ANY binding removed"
fi

echo ""
echo "2. Checking binary compilation..."
if [ -f "build/encoder" ]; then
    echo "   ✓ Binary exists: build/encoder"
    echo "   Binary size: $(ls -lh build/encoder | awk '{print $5}')"
    echo "   Last modified: $(stat -c %y build/encoder | cut -d'.' -f1)"
else
    echo "   ✗ Binary not found. Run: cd build && make"
    exit 1
fi

echo ""
echo "3. Checking configuration..."
if [ -f "config/config.json" ]; then
    RTP_ADDR=$(grep -o '"rtpAddress"[[:space:]]*:[[:space:]]*"[^"]*"' config/config.json | cut -d'"' -f4)
    RTP_PORT=$(grep -o '"rtpPort"[[:space:]]*:[[:space:]]*[0-9]*' config/config.json | grep -o '[0-9]*$')
    RTP_IFACE=$(grep -o '"rtpInterface"[[:space:]]*:[[:space:]]*"[^"]*"' config/config.json | cut -d'"' -f4)
    
    echo "   Configured RTP Address: $RTP_ADDR"
    echo "   Configured RTP Port: $RTP_PORT"
    echo "   Configured RTP Interface: $RTP_IFACE"
    
    # Validate multicast address range
    FIRST_OCTET=$(echo $RTP_ADDR | cut -d'.' -f1)
    if [ "$FIRST_OCTET" -ge 224 ] && [ "$FIRST_OCTET" -le 239 ]; then
        echo "   ✓ Valid multicast address range (224-239)"
    else
        echo "   ⚠ WARNING: Not a standard multicast address"
    fi
else
    echo "   ⚠ Config file not found"
fi

echo ""
echo "4. Network interface check..."
if [ ! -z "$RTP_IFACE" ]; then
    if ip addr show "$RTP_IFACE" &>/dev/null; then
        IP_ADDR=$(ip addr show "$RTP_IFACE" | grep "inet " | awk '{print $2}' | cut -d'/' -f1)
        echo "   ✓ Interface $RTP_IFACE exists"
        echo "   Interface IP: $IP_ADDR"
    else
        echo "   ✗ Interface $RTP_IFACE not found"
    fi
fi

echo ""
echo "=== Fix Verification Complete ==="
echo ""
echo "Expected Behavior:"
echo "  - Socket will bind to: $RTP_ADDR:$RTP_PORT"
echo "  - Will ONLY receive packets destined to $RTP_ADDR"
echo "  - Other multicast streams on port $RTP_PORT will be filtered out"
echo ""
echo "To test:"
echo "  1. Ensure multiple RTP streams exist on port $RTP_PORT"
echo "  2. Run: ./build/encoder --headless --config config/config.json"
echo "  3. Check logs for: 'Subscribed to multicast $RTP_ADDR:$RTP_PORT'"
echo "  4. Verify audio matches the configured stream, not others"
