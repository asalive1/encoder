# AAC Encoder with Web UI

Hello World!!

A high-performance AAC audio encoder with web interface, supporting RTP/multicast input, Icecast streaming, and HLS output. Initially created by Andrew Salive.

## Features

- **Multiple Input Sources**: RTP/multicast, PortAudio devices
- **AAC Encoding**: FDK-AAC library for high-quality encoding
- **Icecast Streaming**: Direct streaming to Icecast servers with metadata
- **HLS Output**: HTTP Live Streaming with configurable segments
- **Web Interface**: Real-time monitoring and control
- **WebSocket API**: Live status updates and control
- **Metadata Integration**: Amperwave station metadata support
- **Docker Support**: Containerized deployment with optimized builds
- **Flexible Configuration**: JSON config, environment variables, CLI arguments

## Requirements

### System Dependencies
- CMake 3.16+
- C++17 compiler (GCC/Clang)
- PortAudio 2.0
- FDK-AAC
- libxml2
- OpenSSL
- libcurl

### Ubuntu/Debian
```bash
apt-get install -y build-essential cmake pkg-config \
    libportaudio2 portaudio19-dev \
    libfdk-aac-dev libxml2-dev \
    libssl-dev libcurl4-openssl-dev
```

## Building

### Standard Build
```bash
mkdir build && cd build
cmake ..
make
```

### Docker Build
```bash
docker build -t aac-encoder .
```

Or use the optimized build:
```bash
docker build -f Dockerfile.optimized -t aac-encoder:optimized .
```

## Configuration

### Configuration Priority
1. CLI argument: `--config /path/to/config.json` (highest)
2. Environment variable: `AAC_ENCODER_CONFIG=/path/to/config.json`
3. Default: `<program_dir>/config/config.json` (lowest)

### Configuration File
```json
{
    "webPort": 8021,
    "iceEnabled": true,
    "icecastUrl": "http://localhost:8000/stream",
    "user": "source",
    "pass": "hackme",
    "icecastInterface": "192.168.4.57",
    "bitrate": 128000,
    "channels": 2,
    "sampleRate": 48000,
    "inputType": "rtp",
    "rtpAddress": "239.192.0.101",
    "rtpPort": 5004,
    "rtpInterface": "192.168.4.57",
    "hlsEnabled": true,
    "hlsSegmentSeconds": 6,
    "hlsWindow": 5
}
```

### Key Configuration Options

| Option | Description | Default |
|--------|-------------|---------|
| `webPort` | Web interface port | 8021 |
| `iceEnabled` | Enable Icecast streaming | true |
| `icecastUrl` | Icecast server URL | - |
| `bitrate` | Audio bitrate (bps) | 128000 |
| `channels` | Audio channels (1/2) | 2 |
| `sampleRate` | Sample rate (Hz) | 48000 |
| `inputType` | Input source (rtp/device) | rtp |
| `rtpAddress` | Multicast address | - |
| `rtpPort` | RTP port | 5004 |
| `rtpInterface` | Network interface IP | - |
| `hlsEnabled` | Enable HLS output | true |
| `hlsSegmentSeconds` | HLS segment duration | 6 |
| `hlsWindow` | HLS playlist window | 5 |

## Usage

### Basic Usage
```bash
./encoder
```

### Headless Mode
```bash
./encoder --headless
```

### Custom Configuration
```bash
./encoder --config /path/to/config.json
```

### Environment Variable
```bash
export AAC_ENCODER_CONFIG="/etc/encoder/config.json"
./encoder --headless
```

### Docker
```bash
docker run -d \
  -p 8021:8021 \
  -v $(pwd)/config:/app/config \
  -v $(pwd)/logs:/app/logs \
  -v $(pwd)/HLS:/app/HLS \
  --network host \
  aac-encoder
```

### Docker Compose
```yaml
version: '3.8'
services:
  encoder:
    image: aac-encoder
    ports:
      - "8021:8021"
    volumes:
      - ./config:/app/config
      - ./logs:/app/logs
      - ./HLS:/app/HLS
    network_mode: host
    environment:
      - AAC_ENCODER_CONFIG=/app/config/config.json
```

## Web Interface

Access the web interface at `http://localhost:8021`

Features:
- Real-time audio level meters
- Stream status monitoring
- Configuration display
- Start/stop controls
- Metadata display

## API

### WebSocket Endpoint
```
ws://localhost:8021/ws
```

Status updates sent every second with:
- Audio levels (L/R)
- Stream status
- Bitrate
- Uptime
- Current metadata

### HTTP Endpoints

#### GET /status
Returns current encoder status
```json
{
  "running": true,
  "bitrate": 128000,
  "sampleRate": 48000,
  "channels": 2,
  "uptime": 3600
}
```

#### POST /start
Start encoding

#### POST /stop
Stop encoding

#### GET /config
Get current configuration

## HLS Output

When enabled, HLS segments are written to the `HLS/` directory:
- `index.m3u8` - Master playlist
- `segments/` - TS segment files

Access via: `http://localhost:8021/hls/index.m3u8`

## Logging

Logs are written to the `logs/` directory with daily rotation:
- Format: `Encoder{YYYY-MM-DD}.log`
- Automatic rotation at midnight
- Configurable via `logDir` in config

## Network Interfaces

The encoder supports both interface names and IP addresses:

```json
{
  "rtpInterface": "eth0",
  "icecastInterface": "192.168.1.100"
}
```

Interface names are automatically resolved to IP addresses.

## Metadata

Supports Amperwave station metadata integration:
- Automatic metadata fetching
- Periodic updates
- Icecast metadata injection
- HLS metadata tags

Configure with:
```json
{
  "amperwaveStationId": "15033",
  "iceMetaEnabled": true,
  "hlsMetaEnabled": true
}
```

## Development

### Project Structure
```
.
├── src/
│   ├── main.cpp          # Main encoder application
│   └── list_devices.cpp  # Audio device listing utility
├── vendor/               # Third-party libraries
│   ├── httplib/         # HTTP server
│   ├── json/            # JSON parsing
│   ├── wsserver/        # WebSocket server
│   └── asio-master/     # Async I/O
└── config/              # Configuration files
```

### Testing

List available audio devices:
```bash
./list_devices
```

Test RTP reception:
```bash
./diagnose_rtp.sh
```

Test configuration:
```bash
./test_env_config.sh
```

## Troubleshooting

### No Audio Input
- Check RTP multicast address and port
- Verify network interface configuration
- Test with `diagnose_rtp.sh`

### Icecast Connection Failed
- Verify Icecast server is running
- Check credentials in config
- Ensure network connectivity

### HLS Not Working
- Check `hlsEnabled` is true
- Verify write permissions on HLS directory
- Check web server is serving HLS files

### Docker Network Issues
- Use `--network host` for multicast
- Ensure proper port mappings
- Check firewall rules

## License

See LICENSE file for details.

## Contributing

Contributions welcome! Please submit pull requests or open issues for bugs and feature requests.

## Version

Current version: 1.0.14

See `version.h` for build information.
