#!/usr/bin/env bash
# RotorLab GC: one-time setup of the Linux release build host (G72).
#
# Run ONCE, as root, in a WSL2 Ubuntu 22.04 distro (the same Ubuntu release as
# the Linux CI runner, ubuntu-22.04, so the AppImage needs no newer glibc than
# 22.04 has):
#
#   wsl -d Ubuntu-22.04 -u root -- bash deploy/rotorlab/linux/setup-build-host.sh
#
# What it does, following RotorLab's Linux CI job:
#   1. apt packages from tools/setup/install-dependencies-debian.sh (the CI step
#      "Install Dependencies"), plus xvfb for the boot test.
#   2. CMake 3.31 and Ninja from PyPI (CI uses lukka/get-cmake; Ubuntu 22.04's
#      own CMake 3.22 is older than the 3.25 GC needs).
#   3. Qt 6.8.3 linux_gcc_64 with the same modules as the CI job, through
#      aqtinstall 3.2.1, into /opt/Qt.
#   4. A normal user "gcbuild" that runs the builds: GC refuses to start as
#      root, and the release build runs GC once (--simple-boot-test).
#
# release.ps1 never runs this script and never installs anything; it stops and
# names this script when the host is not set up.
set -euo pipefail

QT_VERSION=6.8.3
AQT_VERSION=3.2.1
CMAKE_VERSION=3.31.6
NINJA_VERSION=1.11.1.4
QT_MODULES="qtcharts qtlocation qtpositioning qtspeech qt5compat qtmultimedia qtserialport qtimageformats qtshadertools qtconnectivity qtquick3d qtsensors"
TOOLS=/opt/rotorlab-build
BUILD_USER=gcbuild

if [ "$(id -u)" -ne 0 ]; then
    echo "setup-build-host.sh: run as root (wsl -u root)" >&2
    exit 1
fi
. /etc/os-release
echo "== host: $PRETTY_NAME"
if [ "${VERSION_ID:-}" != "22.04" ]; then
    echo "== NOTE: the release recipe is Ubuntu 22.04 (CI runner); this host is $VERSION_ID"
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS="$HERE/../../../tools/setup/install-dependencies-debian.sh"
echo "== apt dependencies: $DEPS"
bash "$DEPS"
DEBIAN_FRONTEND=noninteractive apt-get -y --quiet install xvfb xauth python3-venv file squashfs-tools

echo "== cmake $CMAKE_VERSION, ninja $NINJA_VERSION, aqtinstall $AQT_VERSION in $TOOLS/venv"
python3 -m venv "$TOOLS/venv"
"$TOOLS/venv/bin/pip" install --quiet --upgrade pip
"$TOOLS/venv/bin/pip" install --quiet "cmake==$CMAKE_VERSION" "ninja==$NINJA_VERSION" "aqtinstall==$AQT_VERSION"

echo "== Qt $QT_VERSION linux_gcc_64 into /opt/Qt"
# shellcheck disable=SC2086
"$TOOLS/venv/bin/aqt" install-qt linux desktop "$QT_VERSION" linux_gcc_64 \
    --outputdir /opt/Qt --modules $QT_MODULES

if ! id "$BUILD_USER" >/dev/null 2>&1; then
    useradd --create-home --shell /bin/bash "$BUILD_USER"
fi

cat >"$TOOLS/versions.txt" <<EOF
host:       $PRETTY_NAME
qt:         $QT_VERSION linux_gcc_64 (aqtinstall $AQT_VERSION): $QT_MODULES
cmake:      $("$TOOLS/venv/bin/cmake" --version | head -1)
ninja:      $("$TOOLS/venv/bin/ninja" --version)
gcc:        $(gcc --version | head -1)
glibc:      $(ldd --version | head -1)
EOF
cat "$TOOLS/versions.txt"
echo "== build host ready"
