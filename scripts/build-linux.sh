#!/usr/bin/env bash
#
# Build the Linux omp-mysql.so inside a Debian container.
#
#   ARCH=32 (default) -> i386 (linux/386), build-linux/, glibc 2.31 — the current
#                        open.mp server's arch (its server + components are i386).
#   ARCH=64           -> x86_64 (linux/amd64), build-linux64/ — future-proofing
#                        for a 64-bit open.mp server.
#
# The MariaDB Connector/C submodule is built from source inside the container
# (OpenSSL TLS) and statically linked, so the .so is self-contained. Requires a
# working Docker (e.g. `colima start`).
#
# Usage:  scripts/build-linux.sh            # 32-bit
#         ARCH=64 scripts/build-linux.sh    # 64-bit
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="${ARCH:-32}"

# Bullseye gives glibc 2.31, matching the open.mp Linux server. (Bookworm's glibc
# 2.36 made the .so require GLIBC_2.34 symbols the server can't resolve.)
# Bullseye's apt CMake is 3.18 (<3.19, the SDK's floor) and no runnable newer
# binary exists for the target userland, so CMake is pip-installed — it is only
# the build DRIVER, never linked into the .so, so it has zero effect on the
# artifact's glibc baseline.
IMAGE="debian:bullseye"
if [ "$ARCH" = "64" ]; then
	PLATFORM="linux/amd64"
	BUILD_DIR="build-linux64"
	TOOLCHAIN=""   # native 64-bit; no -m32 toolchain
else
	PLATFORM="linux/386"
	BUILD_DIR="build-linux"
	TOOLCHAIN="-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-i386.cmake"
fi

echo "build-linux: building ${ARCH}-bit omp-mysql.so in $IMAGE ($PLATFORM) -> ${BUILD_DIR}" >&2

docker run --rm --platform "$PLATFORM" \
	-v "$REPO_ROOT:/src" -w /src \
	-e DEBIAN_FRONTEND=noninteractive \
	-e BUILD_DIR="$BUILD_DIR" -e TOOLCHAIN="$TOOLCHAIN" \
	"$IMAGE" bash -euo pipefail -c '
		apt-get update -qq
		# Build deps + the MariaDB Connector/C submodule deps (OpenSSL, zlib).
		apt-get install -y -qq --no-install-recommends \
			build-essential ninja-build python3-pip \
			libssl-dev zlib1g-dev libzstd-dev ca-certificates >/dev/null
		pip3 install --quiet "cmake>=3.19,<4"
		export PATH="/usr/local/bin:$PATH"
		cmake --version | head -1

		# Build in a host-mounted dir so the .so lands on the host.
		rm -rf "$BUILD_DIR"
		cmake -B "$BUILD_DIR" -G Ninja $TOOLCHAIN -DCMAKE_BUILD_TYPE=Release
		cmake --build "$BUILD_DIR"

		echo "=== built artifact ==="
		ls -la "$BUILD_DIR/omp-mysql.so"
		echo "=== file type ==="
		(file "$BUILD_DIR/omp-mysql.so" 2>/dev/null || true)
		echo "=== max GLIBC version referenced (compat check) ==="
		objdump -T "$BUILD_DIR/omp-mysql.so" 2>/dev/null \
			| grep -oE "GLIBC_[0-9]+\.[0-9]+" | sort -uV | tail -5 || true
	'

echo "build-linux: done -> $BUILD_DIR/omp-mysql.so" >&2
