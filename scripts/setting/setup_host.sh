
#!/usr/bin/env bash

set -e

echo "========================================"
echo " Host Development Environment Setup"
echo "========================================"

echo "[1/4] Updating package lists..."
sudo apt update

echo "[2/4] Installing development packages..."

sudo apt install -y \
autoconf \
automake \
bc \
bison \
build-essential \
bzip2 \
cmake \
curl \
file \
flex \
g++ \
gawk \
gcc \
gettext \
git \
gperf \
help2man \
libcamera-dev \
libncurses-dev \
libopencv-dev \
libssl-dev \
libtool \
libtool-bin \
make \
patch \
pkg-config \
python3 \
python3-dev \
python3-pip \
python3-venv \
rsync \
texinfo \
unzip \
wget \
xz-utils \
gcc-14-aarch64-linux-gnu \
g++-14-aarch64-linux-gnu \
binutils-aarch64-linux-gnu \
gdb-multiarch \
sshpass \
asn1c

echo "[3/4] Installing GitHub CLI..."

if command -v gh >/dev/null 2>&1; then
    echo "GitHub CLI already installed."
else
    if apt-cache show gh >/dev/null 2>&1; then
        sudo apt install -y gh
    elif command -v snap >/dev/null 2>&1; then
        sudo snap install gh --classic
    else
        echo "ERROR: GitHub CLI (gh) could not be installed."
        echo "Install it manually from:"
        echo "https://cli.github.com/"
        exit 1
    fi
fi

echo "[4/4] Checking installed versions..."

echo ""

echo "Cross GCC:"
aarch64-linux-gnu-gcc-14 --version | head -n1

echo "Cross G++:"
aarch64-linux-gnu-g++-14 --version | head -n1

echo "GDB:"
gdb-multiarch --version | head -n1

echo "Git:"
git --version

echo "GitHub CLI:"
gh --version | head -n1

echo "Python:"
python3 --version

echo ""
echo "========================================"
echo " Host setup completed successfully."
echo "========================================"
