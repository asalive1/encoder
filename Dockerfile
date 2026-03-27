FROM ubuntu:24.04

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    portaudio19-dev \
    libxml2-dev \
    libssl-dev \
    libfdk-aac-dev \
    libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source code
COPY . .

# Build application
RUN mkdir -p build && cd build && \
    cmake .. && \
    make encoder

# Create runtime directories
# Config is resolved relative to the binary at /app/build, so config lives at /app/build/config
# These directories are overridden at runtime by docker-compose volume mounts from the host
RUN mkdir -p /app/build/config /app/logs /app/HLS /app/www && \
    chmod -R 777 /app/build/config /app/logs /app/HLS /app/www

# Run as root so the encoder can write to host-mounted volumes regardless of host directory ownership
# (config saves, log rotation, HLS segment writes all require write access)

# Expose web UI port (matches default webPort in config.json)
EXPOSE 8020

# Config path resolved relative to binary directory (/app/build)
ENV AAC_ENCODER_CONFIG=/app/build/config/config.json

# Default command — web UI enabled (no --headless)
CMD ["./build/encoder"]
