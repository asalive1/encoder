#!/bin/bash

echo "=== Docker Containerization Test ==="

cd /mnt/data/projects/poc/axia/converter/AACencoder-v1.0.12-SourceCode-fixed1/AACencoder-v1.0.12-SourceCode

# Test 1: Check Dockerfile syntax
echo "1. Testing Dockerfile syntax..."
docker build --dry-run . >/dev/null 2>&1 && echo "   ✅ Dockerfile syntax OK" || echo "   ❌ Dockerfile syntax error"

# Test 2: Check .dockerignore
echo "2. Testing .dockerignore..."
[ -f .dockerignore ] && echo "   ✅ .dockerignore exists" || echo "   ❌ .dockerignore missing"

# Test 3: Check docker-compose.yml
echo "3. Testing docker-compose.yml..."
docker compose config >/dev/null 2>&1 && echo "   ✅ docker-compose.yml valid" || echo "   ❌ docker-compose.yml invalid"

# Test 4: Check deployment script
echo "4. Testing deployment script..."
[ -x docker-deploy.sh ] && echo "   ✅ Deployment script executable" || echo "   ❌ Deployment script not executable"

# Test 5: Check sample config
echo "5. Testing sample config..."
[ -f config/docker-config.json ] && echo "   ✅ Sample config exists" || echo "   ❌ Sample config missing"

echo -e "\n=== Docker Files Created ==="
echo "📁 Dockerfile              - Standard build image"
echo "📁 Dockerfile.optimized    - Multi-stage optimized image"
echo "📁 docker-compose.yml      - Compose deployment"
echo "📁 .dockerignore           - Build context optimization"
echo "📁 docker-deploy.sh        - Deployment automation"
echo "📁 config/docker-config.json - Sample configuration"
echo "📁 DOCKER.md               - Complete documentation"

echo -e "\n=== Quick Start Commands ==="
echo "# Build and run:"
echo "./docker-deploy.sh build"
echo "./docker-deploy.sh run"
echo ""
echo "# Or use docker compose:"
echo "docker compose up -d"
echo ""
echo "# Access web UI:"
echo "open http://localhost:8080"
