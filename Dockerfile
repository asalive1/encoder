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
RUN mkdir -p /app/config /app/logs /app/HLS && \
    chown -R 1000:1000 /app && \
    chmod -R 755 /app

# Expose web UI port
EXPOSE 8080

# Switch to non-root user
USER 1000

# Set default config via environment variable
ENV AAC_ENCODER_CONFIG=/app/config/config.json

# Default command
CMD ["./build/encoder", "--headless"]
