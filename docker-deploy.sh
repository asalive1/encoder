#!/bin/bash

set -e

IMAGE_NAME="aac-encoder"
VERSION="1.0.14"

echo "=== AAC Encoder Docker Deployment ==="

# Build options
case "${1:-build}" in
    "build")
        echo "Building standard Docker image..."
        docker build -t ${IMAGE_NAME}:${VERSION} -t ${IMAGE_NAME}:latest .
        ;;
    "build-optimized")
        echo "Building optimized Docker image..."
        docker build -f Dockerfile.optimized -t ${IMAGE_NAME}:${VERSION}-slim -t ${IMAGE_NAME}:slim .
        ;;
    "run")
        echo "Running container..."
        docker run -d \
            --name aac-encoder \
            -p 8080:8080 \
            -v $(pwd)/config:/app/config \
            -v $(pwd)/logs:/app/logs \
            -v $(pwd)/HLS:/app/HLS \
            -e AAC_ENCODER_CONFIG=/app/config/docker-config.json \
            ${IMAGE_NAME}:latest
        echo "Container started. Web UI: http://localhost:8080"
        ;;
    "compose")
        echo "Starting with docker compose..."
        docker compose up -d
        echo "Services started. Web UI: http://localhost:8080"
        ;;
    "stop")
        echo "Stopping container..."
        docker stop aac-encoder 2>/dev/null || true
        docker rm aac-encoder 2>/dev/null || true
        docker compose down 2>/dev/null || true
        ;;
    "logs")
        echo "Showing container logs..."
        if docker ps --format "table {{.Names}}" | grep -q aac-encoder; then
            docker logs -f aac-encoder
        else
            docker compose logs -f
        fi
        ;;
    "shell")
        echo "Opening shell in container..."
        docker exec -it aac-encoder /bin/bash
        ;;
    *)
        echo "Usage: $0 {build|build-optimized|run|compose|stop|logs|shell}"
        echo ""
        echo "Commands:"
        echo "  build           - Build standard Docker image"
        echo "  build-optimized - Build optimized multi-stage image"
        echo "  run             - Run container with volume mounts"
        echo "  compose         - Start with docker compose"
        echo "  stop            - Stop and remove container"
        echo "  logs            - Show container logs"
        echo "  shell           - Open shell in running container"
        exit 1
        ;;
esac

echo "✅ Operation completed successfully"
