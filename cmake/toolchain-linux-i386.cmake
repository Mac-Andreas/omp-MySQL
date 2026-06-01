# CMake toolchain: build a 32-bit Linux mysql.so (ELF i386 / glibc).
#
# The open.mp Linux server and its components are 32-bit (ELF 32-bit LSB,
# Intel 80386, ld-linux.so.2), so the component must be i386. Linux uses the
# Itanium C++ ABI for both gcc and clang, so no special ABI handling is needed
# (unlike the Windows/MSVC case) — only -m32 and a 32-bit libmysqlclient.
#
# Intended to run INSIDE a 32-bit Linux build container (see scripts/build-linux.sh),
# where gcc/g++ already default to i386 and the 32-bit client lib is installed.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)

# Force 32-bit codegen even if the host compiler is multilib/64-bit-capable.
set(CMAKE_C_FLAGS_INIT   "-m32")
set(CMAKE_CXX_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-m32")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-m32")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-m32")

# Same glm SIMD note as the other targets: "x86" => GLM_FORCE_SSE2, not NEON.
# (CMAKE_SYSTEM_PROCESSOR i686 already won't match the SDK's x86 regex... so set
# it explicitly to a matching token.)
set(CMAKE_SYSTEM_PROCESSOR x86)

# The MySQL client is the MariaDB Connector/C submodule, built from source by
# the top-level CMakeLists (static, OpenSSL TLS). The container provides
# libssl-dev/zlib1g-dev; the connector is statically linked into mysql.so, so no
# connector rpath is needed.
