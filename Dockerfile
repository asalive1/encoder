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

# Create runtime user (use existing ubuntu user if UID 1000 exists)
RUN id -u 1000 >/dev/null 2>&1 || useradd -r -s /bin/false -u 1000 encoder

# Create directories and set permissions
# Config lives at /app/build/config — relative to the encoder binary location
RUN mkdir -p /app/build/config /app/logs /app/HLS && \
    chown -R 1000:1000 /app && \
    chmod -R 755 /app

# Expose web UI port (default webPort in config.json)
EXPOSE 8020

# Switch to non-root user
USER 1000

# Config path resolved relative to binary dir (/app/build)
ENV AAC_ENCODER_CONFIG=/app/build/config/config.json

# Default command — no --headless so the web UI is available
CMD ["./build/encoder"]
