cmake_minimum_required(VERSION 3.19)
project(omp-mysql LANGUAGES C CXX VERSION 1.0.0)

# --- Standards -------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Some vendored SDK sub-libraries (e.g. glm) still declare a very old
# cmake_minimum_required; allow them to configure on modern CMake.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# --- open.mp SDK (git submodule; header-only INTERFACE target OMP-SDK) ------
add_subdirectory(sdk)

# On Windows these must be set BEFORE the connector's add_subdirectory: the
# connector only defines them itself when NOT a subproject (IS_SUBPROJECT guard),
# and without WIN32_LEAN_AND_MEAN, <windows.h> drags in the old winsock.h and
# clashes with the connector's <winsock2.h> (sockaddr redefinition).
if(WIN32)
	add_definitions(-DWIN32_LEAN_AND_MEAN -DNOGDI)
endif()

# --- MySQL C API: MariaDB Connector/C (git submodule, built from source) ----
# We build the connector from source so we get a modern, OpenSSL TLS
# 1.3-capable client on 32-bit (the open.mp server's arch). Oracle's old 32-bit
# Connector/C 6.1.11 uses yaSSL (no modern TLS) and Oracle's modern source
# hard-blocks 32-bit, so MariaDB Connector/C is the client — the same choice the
# mature SA-MP-MySQL converged on. We link the STATIC mariadbclient so the
# component needs no separate client .dll/.so beside it.
set(WITH_UNIT_TESTS    OFF CACHE BOOL "" FORCE)
set(WITH_CURL          OFF CACHE BOOL "" FORCE)  # not needed; avoids a curl dep
set(WITH_EXTERNAL_ZLIB OFF CACHE BOOL "" FORCE)
# Force OpenSSL as the TLS backend on EVERY platform (incl. Windows). The
# connector defaults to Schannel on Windows, but wine's Schannel can't complete
# a modern TLS handshake, and one TLS stack everywhere is simpler to ship/test.
# OpenSSL works on real Windows too; we bundle libssl/libcrypto next to the DLL.
set(WITH_SSL           OPENSSL CACHE STRING "" FORCE)
set(INSTALL_LIBDIR     lib CACHE STRING "" FORCE)
# Build the auth plugins STATICally (baked into mariadbclient) so the component
# is self-contained and the connector's dynamic-plugin INSTALL() rules — which
# error out when embedded via add_subdirectory — are never hit. caching_sha2 +
# native + sha256 cover MySQL 8/9 auth; dialog for cleartext/PAM prompts.
# Auth + transport + compression plugins we actually use: STATIC (baked in).
foreach(_plugin
		CACHING_SHA2_PASSWORD MYSQL_NATIVE_PASSWORD SHA256_PASSWORD
		DIALOG CLIENT_ED25519 AUTH_GSSAPI_CLIENT PARSEC
		MYSQL_CLEAR_PASSWORD PVIO_SOCKET PVIO_SHMEM PVIO_NPIPE)
	set(CLIENT_PLUGIN_${_plugin} STATIC CACHE STRING "" FORCE)
endforeach()
# Plugins we don't need: OFF so they aren't built. AURORA/REPLICATION/REMOTEIO
# (connection routing; replication.c also trips strict clang); ZSTD (its bundled
# zstd .c.o objects confuse lld-link, and we don't need zstd wire compression).
foreach(_plugin AURORA REPLICATION REMOTEIO ZSTD)
	set(CLIENT_PLUGIN_${_plugin} OFF CACHE STRING "" FORCE)
endforeach()
set(WITH_ZSTD OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/mariadb-connector-c EXCLUDE_FROM_ALL)

# Present the connector under the names the component already uses.
set(MySQL_FOUND TRUE)
set(MySQL_INCLUDE_DIRS
	"${CMAKE_CURRENT_SOURCE_DIR}/libs/mariadb-connector-c/include"
	"${CMAKE_CURRENT_BINARY_DIR}/libs/mariadb-connector-c/include")
set(MySQL_LIBRARIES mariadbclient)

# pawn-natives is header-only (SCRIPT_API registration); add include paths.
include_directories(
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/pawn-natives
	${CMAKE_CURRENT_SOURCE_DIR}/pawn/source
	${CMAKE_CURRENT_SOURCE_DIR}/pawn/source/amx
	${CMAKE_CURRENT_SOURCE_DIR}/pawn/source/linux
)

add_definitions(
	-DHAVE_STDINT_H=1
	-DPAWN_CELL_SIZE=32
)

if(WIN32)
	# The vendored Pawn AMX headers default HAVE_ALLOCA_H=1 for GNUC/Clang, but on
	# Windows there is no <alloca.h> (it's <malloc.h> / _alloca). Force it off so
	# amx.h takes its Windows path (alloca -> _alloca). (WIN32_LEAN_AND_MEAN is
	# already set above, before the connector subdirectory.)
	add_definitions(-DHAVE_ALLOCA_H=0)
endif()

# Source list grows per phase.
# --- Vendored Argon2 (password hashing) ------------------------------------
# Reference implementation (phc-winner-argon2). We compile the portable C
# sources directly (no SIMD opt.c) into the component.
set(ARGON2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/argon2")
add_library(argon2 STATIC
	${ARGON2_DIR}/src/argon2.c
	${ARGON2_DIR}/src/core.c
	${ARGON2_DIR}/src/encoding.c
	${ARGON2_DIR}/src/ref.c
	${ARGON2_DIR}/src/thread.c
	${ARGON2_DIR}/src/blake2/blake2b.c
)
target_include_directories(argon2 PUBLIC ${ARGON2_DIR}/include)
target_include_directories(argon2 PRIVATE ${ARGON2_DIR}/src)
set_property(TARGET argon2 PROPERTY POSITION_INDEPENDENT_CODE ON)

add_library(${PROJECT_NAME} SHARED
	src/main.cpp
	src/MySQLComponent.cpp
	src/Connection.cpp
	src/QueryQueue.cpp
	src/Statement.cpp
	src/Hashing.cpp
	src/Model.cpp
	src/natives.cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE argon2)

# Components are loaded by exact file name; drop the "lib" prefix on *nix.
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

target_link_libraries(${PROJECT_NAME} PRIVATE OMP-SDK)
# Link the MariaDB Connector/C static client (built from the submodule above).
# The target carries its own include dirs + TLS (OpenSSL) link deps.
target_link_libraries(${PROJECT_NAME} PRIVATE ${MySQL_LIBRARIES})
target_include_directories(${PROJECT_NAME} PRIVATE ${MySQL_INCLUDE_DIRS})
