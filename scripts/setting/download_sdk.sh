#!/usr/bin/env bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

source "$SCRIPT_DIR/project.conf"

SDK_ARCHIVE_NAME="aarch64-buildroot-linux-gnu_sdk-buildroot_rpi5.tar.gz"

echo "========================================"
echo "         SDK Download Utility"
echo "========================================"

#--------------------------------------------------
# Check GitHub CLI
#--------------------------------------------------
if ! command -v gh >/dev/null 2>&1; then
    echo "ERROR: GitHub CLI (gh) is not installed."
    exit 1
fi

#--------------------------------------------------
# Check GitHub Authentication
#--------------------------------------------------
if ! gh auth status >/dev/null 2>&1; then
    echo "GitHub authentication is required."
    echo ""
    echo "Please prepare a Personal Access Token (PAT)."
    echo ""
    echo "Authentication method:"
    echo "  GitHub.com"
    echo "  HTTPS"
    echo "  Paste an authentication token"
    echo ""

    gh auth login

    echo ""

    if ! gh auth status >/dev/null 2>&1; then
        echo "ERROR: GitHub authentication failed."
        exit 1
    fi
fi

#--------------------------------------------------
# Find Latest Release
#--------------------------------------------------
echo "[1/5] Finding latest release..."

LATEST_TAG=$(gh release list --limit 1 --json tagName --jq '.[0].tagName')

if [ -z "$LATEST_TAG" ]; then
    echo "ERROR: No GitHub Release found."
    exit 1
fi

echo "Latest Release : $LATEST_TAG"

SDK_PATH="$PROJECT_ROOT/$SDK_DIR"

#--------------------------------------------------
# Existing SDK
#--------------------------------------------------
if [ -d "$SDK_PATH" ]; then
    echo ""
    read -rp "Existing SDK found. Remove it? (y/N): " ANSWER

    if [[ "$ANSWER" =~ ^[Yy]$ ]]; then
        rm -rf "$SDK_PATH"
    else
        echo "Canceled."
        exit 0
    fi
fi

mkdir -p "$SDK_PATH"

#--------------------------------------------------
# Download SDK
#--------------------------------------------------
echo "[2/5] Downloading SDK..."

gh release download "$LATEST_TAG" \
    --pattern "$SDK_ARCHIVE_NAME" \
    --dir "$SDK_PATH"

#--------------------------------------------------
# Extract SDK
#--------------------------------------------------
echo "[3/5] Extracting SDK..."

SDK_ARCHIVE="$SDK_PATH/$SDK_ARCHIVE_NAME"

if [ ! -f "$SDK_ARCHIVE" ]; then
    echo "ERROR: SDK archive not found."
    exit 1
fi

tar -xpf "$SDK_ARCHIVE" \
    -C "$SDK_PATH" \
    --strip-components=1

#--------------------------------------------------
# Cleanup
#--------------------------------------------------
echo "[4/5] Cleaning up..."

rm -f "$SDK_ARCHIVE"

#--------------------------------------------------
# Done
#--------------------------------------------------
echo "[5/5] Completed."

echo ""
echo "========================================"
echo "SDK successfully installed."
echo ""
echo "Next step:"
echo ""
echo "source scripts/setting/setup_sdk.sh"
echo "========================================"
