#!/bin/bash
# Setup GitHub Container Registry authentication for docker-compose

set -e

echo "=== GitHub Container Registry Authentication Setup ==="
echo ""
echo "This script will help you authenticate with GHCR to pull the encoder image."
echo ""
echo "You'll need:"
echo "1. Your GitHub username"
echo "2. A Personal Access Token (PAT) with 'read:packages' scope"
echo "   Generate one at: https://github.com/settings/tokens"
echo ""

read -p "Enter your GitHub username: " GITHUB_USERNAME
read -sp "Enter your Personal Access Token (will not be echoed): " GITHUB_TOKEN
echo ""

echo ""
echo "Authenticating with GitHub Container Registry..."

# Login to GitHub Container Registry
echo "$GITHUB_TOKEN" | docker login ghcr.io -u "$GITHUB_USERNAME" --password-stdin

if [ $? -eq 0 ]; then
    echo ""
    echo "✓ Authentication successful!"
    echo ""
    echo "Your credentials are stored in ~/.docker/config.json"
    echo "You can now run 'docker-compose up' to pull the image."
else
    echo ""
    echo "✗ Authentication failed. Please check your credentials."
    exit 1
fi
