# CMake toolchain: cross-compile the component to a 32-bit Windows mysql.dll
# using clang-cl + lld-link against a real MSVC + Windows SDK tree.
#
# WHY clang-cl and not mingw: open.mp components must use the MSVC C++ ABI. A
# mingw/gcc DLL *loads* but its IComponent vtable is laid out with the Itanium
# ABI, so omp-server (MSVC ABI) misreads name/UID/version and crashes. clang-cl
# produces MSVC-ABI objects, so the vtable matches.
#
# Requires an MSVC + Windows SDK tree (e.g. via `xwin`, or a copied VS install)
# at MSVC_BASE below, and Homebrew LLVM (clang-cl/lld-link/llvm-lib).
#
# Usage:
#   cmake -B build-win32 \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-clang-cl-win32.cmake
#   cmake --build build-win32 --config Release

set(CMAKE_SYSTEM_NAME Windows)
# "x86" so the SDK's glm SIMD select picks GLM_FORCE_SSE2, not <arm_neon.h>.
set(CMAKE_SYSTEM_PROCESSOR x86)

set(_llvm /opt/homebrew/opt/llvm/bin)
set(CMAKE_C_COMPILER   "${_llvm}/clang-cl")
set(CMAKE_CXX_COMPILER "${_llvm}/clang-cl")
set(CMAKE_LINKER       "${_llvm}/lld-link")
set(CMAKE_AR           "${_llvm}/llvm-lib")
set(CMAKE_RC_COMPILER  "${_llvm}/llvm-rc")
set(CMAKE_C_COMPILER_TARGET   i686-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET i686-pc-windows-msvc)

# clang-cl drives linking through lld-link.
set(CMAKE_LINKER_TYPE LLD)

# --- MSVC + Windows SDK locations -----------------------------------------
# An MSVC CRT + Windows SDK tree (e.g. produced by `xwin splat`, or copied from a
# VS install). Override with -DMSVC_BASE=... / -DMSVC_VER=... / -DWINSDK_VER=...
# or the matching env vars. Defaults match a local xwin/VS layout under ~/msvc.
if(NOT DEFINED MSVC_BASE)
	if(DEFINED ENV{MSVC_BASE})
		set(MSVC_BASE "$ENV{MSVC_BASE}")
	else()
		set(MSVC_BASE "$ENV{HOME}/msvc")
	endif()
endif()
if(NOT DEFINED MSVC_VER)
	set(MSVC_VER "14.51.36231")
endif()
if(NOT DEFINED WINSDK_VER)
	set(WINSDK_VER "10.0.26100.0")
endif()

if(NOT EXISTS "${MSVC_BASE}/VC/Tools/MSVC/${MSVC_VER}/include/vcruntime.h")
	message(FATAL_ERROR
		"MSVC toolchain not found at ${MSVC_BASE} (VC ${MSVC_VER}). "
		"Set -DMSVC_BASE / -DMSVC_VER / -DWINSDK_VER, or fetch one with xwin. "
		"This build needs the MSVC C++ ABI — open.mp components built with mingw "
		"load but crash omp-server (vtable ABI mismatch).")
endif()

set(_vc      "${MSVC_BASE}/VC/Tools/MSVC/${MSVC_VER}")
set(_sdk_inc "${MSVC_BASE}/Windows Kits/10/Include/${WINSDK_VER}")
set(_sdk_lib "${MSVC_BASE}/Windows Kits/10/Lib/${WINSDK_VER}")

# Include search (MSVC CRT + UCRT + Win32 um + shared). /imsvc = system include
# (suppresses warnings from SDK headers).
foreach(_inc "${_vc}/include" "${_sdk_inc}/ucrt" "${_sdk_inc}/um" "${_sdk_inc}/shared")
	string(APPEND _winsdk_inc_flags " /imsvc \"${_inc}\"")
endforeach()

# winsysroot-less: feed include + lib dirs explicitly. 32-bit => x86 libs.
set(_common_flags "${_winsdk_inc_flags} -m32 -fms-compatibility -fms-extensions")
set(CMAKE_C_FLAGS_INIT   "${_common_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_common_flags}")

set(_libpaths "/libpath:\"${_vc}/lib/x86\" /libpath:\"${_sdk_lib}/ucrt/x86\" /libpath:\"${_sdk_lib}/um/x86\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_libpaths}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_libpaths}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_libpaths}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# The MySQL client is the MariaDB Connector/C submodule, built from source by
# the top-level CMakeLists (OpenSSL TLS on every platform). No connector pinning
# here. For a local Windows cross-build you must point the connector at a Windows
# i386 OpenSSL (e.g. -DOPENSSL_ROOT_DIR=<win32-openssl>); otherwise find_package
# may grab the host's macOS OpenSSL. CI builds use vcpkg (see build.yml), which is
# the supported/shipping Windows path.
