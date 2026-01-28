#!/bin/bash
# RTP Multicast Diagnostic Tool
# Helps verify the fix is working correctly in production

echo "╔════════════════════════════════════════════════════════════╗"
echo "║        RTP Multicast Stream Diagnostic Tool               ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Read config
if [ -f "config/config.json" ]; then
    RTP_ADDR=$(grep -o '"rtpAddress"[[:space:]]*:[[:space:]]*"[^"]*"' config/config.json | cut -d'"' -f4)
    RTP_PORT=$(grep -o '"rtpPort"[[:space:]]*:[[:space:]]*[0-9]*' config/config.json | grep -o '[0-9]*$')
    RTP_IFACE=$(grep -o '"rtpInterface"[[:space:]]*:[[:space:]]*"[^"]*"' config/config.json | cut -d'"' -f4)
else
    echo -e "${RED}✗ Config file not found${NC}"
    exit 1
fi

echo "Configuration:"
echo "  RTP Address: $RTP_ADDR"
echo "  RTP Port: $RTP_PORT"
echo "  RTP Interface: $RTP_IFACE"
echo ""

# Check if fix is applied
echo "1. Verifying Fix Status..."
if grep -q "inet_addr(cfg.rtpAddress.c_str())" src/main.cpp; then
    echo -e "   ${GREEN}✓ Fix is applied${NC} - Binding to specific multicast address"
else
    echo -e "   ${RED}✗ Fix NOT applied${NC} - Still using INADDR_ANY"
    echo "   Run: Apply the fix from RTP_ADDRESS_FIX.md"
    exit 1
fi
echo ""

# Check for multicast traffic on the network
echo "2. Scanning for RTP Multicast Streams on port $RTP_PORT..."
echo "   (This requires root/sudo and tcpdump)"
echo ""

if command -v tcpdump &> /dev/null; then
    echo "   Listening for 5 seconds..."
    STREAMS=$(timeout 5 sudo tcpdump -i any -n "udp port $RTP_PORT and dst net 224.0.0.0/4" 2>/dev/null | \
              grep -oP '\d+\.\d+\.\d+\.\d+ > \d+\.\d+\.\d+\.\d+' | \
              awk '{print $3}' | cut -d':' -f1 | sort -u)
    
    if [ ! -z "$STREAMS" ]; then
        echo ""
        echo "   Detected multicast streams on port $RTP_PORT:"
        while IFS= read -r stream; do
            if [ "$stream" == "$RTP_ADDR" ]; then
                echo -e "     ${GREEN}→ $stream (CONFIGURED STREAM)${NC}"
            else
                echo -e "     ${YELLOW}→ $stream (other stream)${NC}"
            fi
        done <<< "$STREAMS"
        
        # Count streams
        STREAM_COUNT=$(echo "$STREAMS" | wc -l)
        if [ "$STREAM_COUNT" -gt 1 ]; then
            echo ""
            echo -e "   ${YELLOW}⚠ Multiple streams detected!${NC}"
            echo "   This is the scenario where the fix is critical."
        fi
    else
        echo -e "   ${YELLOW}⚠ No multicast traffic detected${NC}"
        echo "   Either no streams are active or tcpdump needs sudo"
    fi
else
    echo -e "   ${YELLOW}⚠ tcpdump not available${NC}"
    echo "   Install with: sudo apt-get install tcpdump"
fi
echo ""

# Check if encoder is running
echo "3. Checking Encoder Status..."
if pgrep -f "encoder.*headless" > /dev/null; then
    PID=$(pgrep -f "encoder.*headless")
    echo -e "   ${GREEN}✓ Encoder is running${NC} (PID: $PID)"
    
    # Check socket bindings
    if command -v ss &> /dev/null; then
        echo ""
        echo "   Socket bindings:"
        sudo ss -anup | grep ":$RTP_PORT" | grep "$PID" | while read line; do
            echo "     $line"
        done
    fi
else
    echo -e "   ${YELLOW}⚠ Encoder is not running${NC}"
fi
echo ""

# Check recent logs
echo "4. Recent Log Entries..."
if [ -d "logs" ]; then
    LATEST_LOG=$(ls -t logs/*.log 2>/dev/null | head -1)
    if [ ! -z "$LATEST_LOG" ]; then
        echo "   Latest log: $LATEST_LOG"
        echo ""
        echo "   RTP-related entries:"
        grep "\[RTP\]" "$LATEST_LOG" | tail -10 | while read line; do
            if echo "$line" | grep -q "Successfully Connected"; then
                echo -e "     ${GREEN}$line${NC}"
            elif echo "$line" | grep -q "Error\|Failed"; then
                echo -e "     ${RED}$line${NC}"
            else
                echo "     $line"
            fi
        done
    else
        echo -e "   ${YELLOW}⚠ No log files found${NC}"
    fi
else
    echo -e "   ${YELLOW}⚠ Logs directory not found${NC}"
fi
echo ""

# Summary
echo "╔════════════════════════════════════════════════════════════╗"
echo "║                      SUMMARY                               ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""
echo "Expected Behavior with Fix:"
echo "  • Socket binds to: $RTP_ADDR:$RTP_PORT"
echo "  • Kernel filters packets at network layer"
echo "  • Only packets to $RTP_ADDR are delivered to application"
echo "  • Other multicast streams on port $RTP_PORT are ignored"
echo ""
echo "To verify correct operation:"
echo "  1. Check logs show: 'Subscribed to multicast $RTP_ADDR:$RTP_PORT'"
echo "  2. Verify audio matches the configured stream"
echo "  3. If multiple streams exist, confirm others are ignored"
echo ""
echo "If issues persist:"
echo "  • Check network interface is correct: $RTP_IFACE"
echo "  • Verify multicast routing: ip mroute show"
echo "  • Check firewall rules: iptables -L -n"
echo "  • Review full logs in: logs/"
echo ""
