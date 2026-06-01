<div align="center">

# omp-MySQL

**A modern, secure MySQL database component for [open.mp](https://github.com/openmultiplayer/open.mp).**

Clean-room. Mandatory TLS. Prepared statements. Argon2id password hashing.
Tested live on MySQL 5.7 → 9.2.

[![build](https://github.com/Mac-Andreas/omp-MySQL/actions/workflows/build.yml/badge.svg)](https://github.com/Mac-Andreas/omp-MySQL/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/Mac-Andreas/omp-MySQL?label=release)](https://github.com/Mac-Andreas/omp-MySQL/releases/latest)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-informational)](#install)
[![MySQL](https://img.shields.io/badge/MySQL-5.7%20%E2%86%92%209.x-orange)](#mysql-server-support)
[![wiki](https://img.shields.io/badge/docs-wiki-success)](https://github.com/Mac-Andreas/omp-MySQL/wiki)

</div>

---

## TL;DR

```pawn
#include <open.mp>
#include <omp-mysql>

new MySQL:g_DB;

public OnGameModeInit()
{
    new MySQLConfig:cfg = mysql_config_create();
    mysql_config_set(cfg, SSL_MODE, SSL_MODE_VERIFY_CA);   // mandatory TLS
    mysql_config_set(cfg, SSL_CA, "scriptfiles/ca.pem");
    g_DB = mysql_connect("127.0.0.1", "user", "${DB_PASS}", "database", cfg);
    if (g_DB == MYSQL_INVALID_HANDLE) print("MySQL connection failed.");
    return 1;
}
```

1. Drop `omp-mysql.dll` / `omp-mysql.so` (+ the bundled `libssl`/`libcrypto`) into `components/`.
2. Put `omp-mysql.inc` in `qawno/include/` and `#include <omp-mysql>`.
3. Connect. The MySQL client is statically linked — **no separate client library to install.**

Full guides, flowcharts, and a from-scratch tutorial are in the **[Wiki](https://github.com/Mac-Andreas/omp-MySQL/wiki)**.

---

## What it is

`open server` ── `omp-MySQL` ── `MySQL server`

A native open.mp **component** (drops into `components/`, not a legacy plugin) that lets
your Pawn gamemode talk to a MySQL database — **safely**. It's a clean-room, ground-up
project inspired by the classic [SA-MP-MySQL](https://github.com/pBlueG/SA-MP-MySQL)
plugin, rebuilt for modern MySQL and the open.mp Component SDK with an
industry-standard (JDBC / DB-API style) API.

- **Author:** Xyranaut · **Org:** Mac Andreas
- **License:** [MIT](LICENSE) — use it anywhere, including closed-source gamemodes.
- **Ships:** Windows `.dll` + Linux `.so` (32-bit, the open.mp server arch).

### Highlights

- 🔒 **Mandatory TLS, fail-closed** — TLS 1.3 (8.0+) / TLS 1.2 (5.7); never falls back to plaintext.
- 🧷 **Prepared statements** — bound parameters, injection-safe by construction.
- 🔑 **Argon2id password hashing** built in (`mysql_hash` / `mysql_verify`).
- ⚡ **Fully async** — one worker thread per connection; the main tick never blocks.
- 🧱 **Self-contained** — MariaDB Connector/C statically linked; OpenSSL bundled.
- 🧪 **Live-tested** on real MySQL 5.7 → 9.2 (see the matrix below).
- 🛡️ **Hardening** — multi-statement off by default, opt-in rate-limiting, `${ENV}` secrets.
- 🧰 **Extras** the old plugin never had: result cache, active-record models, and
  MySQL 9 **VECTOR** similarity search.

---

## MySQL server support

Every row below was **live-tested** — the real component, driving a TLS connection,
a query, a **prepared statement**, and `caching_sha2_password` auth against that exact
server. `BlueG` = the legacy SA-MP-MySQL (discontinued ~2019, MySQL 5.x era) for context.

| MySQL | Released | TLS | Query | Prepared stmt | Auth | VECTOR | BlueG |
|---|---|:---:|:---:|:---:|:---:|:---:|:---:|
| **9.x** (tested 9.2) | 2024–25 | ✅ 1.3 | ✅ | ✅ | ✅ | ✅ | ❌ |
| **8.4 LTS** ⭐ | 2024 | ✅ 1.3 | ✅ | ✅ | ✅ | — | ⚠️ |
| 8.0 | 2018 | ✅ 1.3 | ✅ | ✅ | ✅ | — | ⚠️ |
| **5.7** (minimum) | 2015 | ✅ 1.2 | ✅ | ✅ | ✅ | — | ✅ |
| 5.6 / 5.5 | 2013/10 | ❌ | — | — | — | — | ✅ |

- **Minimum = MySQL 5.7** — the oldest whose default build ships TLS (it auto-generates
  certs and negotiates TLS 1.2). **8.0+ get TLS 1.3.**
- **Recommended = MySQL 8.4 LTS** (production-stable Long-Term Support).
- **VECTOR** (the `VECTOR` datatype / similarity search) is **MySQL 9.0+ only** — it
  simply doesn't exist on earlier servers. Everything else works 5.7 → 9.x.
- **5.6 / 5.5 fail by design**: their default server ships `have_ssl = DISABLED` (no TLS
  at all), and omp-MySQL is fail-closed — it refuses an unencrypted link.

> There is **no MySQL 6.x or 7.x** — Oracle skipped those numbers; 5.7 went to 8.0.

---

## Why a new component? (vs. SA-MP-MySQL)

|  | SA-MP-MySQL (BlueG) | **omp-MySQL** |
|---|---|---|
| Target | SA-MP plugin ABI | native **open.mp Component SDK** |
| MySQL era | 5.x / MariaDB | **modern 8.x / 9.x** (caching_sha2, utf8mb4) |
| TLS | optional / off | **mandatory, fail-closed** (TLS 1.3/1.2) |
| Password hashing | none built in | **Argon2id** (`mysql_hash`/`mysql_verify`) |
| Injection safety | escape helpers | **prepared statements** + `%e` escaping + multi-stmt off |
| API naming | SA-MP-MySQL specific | **industry-standard** (JDBC/DB-API vocabulary) |
| Secrets | in source | **`${ENV}` expansion**, optional obfuscated config |
| Extras | cache, ORM | cache, active-record models, **VECTOR** search, compression |
| Status | discontinued (~2019) | **active, v1.0.0** |

The API does **not** reuse SA-MP-MySQL's native names — it's a modern design
(`mysql_connect` / `mysql_prepare` / `mysql_stmt_set_*` / `mysql_rs_get_*`). A full
old→new mapping for migrating an existing gamemode is in the
**[Migration guide](https://github.com/Mac-Andreas/omp-MySQL/wiki/Migrating-from-SA-MP-MySQL)**.

---

## Install

1. Download the [latest release](https://github.com/Mac-Andreas/omp-MySQL/releases/latest):
   - **Windows:** `omp-mysql.dll` + `libssl-3.dll` + `libcrypto-3.dll` → `components/`
   - **Linux:** `omp-mysql.so` → `components/`
2. `omp-mysql.inc` → `qawno/include/`, then `#include <omp-mysql>`.
3. Connect (see [TL;DR](#tldr)). A complete **login/register admin demo** that exercises
   every native ships as `filterscripts/omp-admin.pwn` — see the
   [omp-admin walkthrough](https://github.com/Mac-Andreas/omp-MySQL/wiki/omp-admin-demo).

---

## Build from source

```sh
git clone --recurse-submodules https://github.com/Mac-Andreas/omp-MySQL
cd omp-MySQL
```

open.mp servers/components are **32-bit (i386)**, so the shipping binaries are i386.
The MySQL client (MariaDB Connector/C) is a submodule built from source and statically
linked — builds need **no external MySQL client install**.

| Target | Command | Notes |
|---|---|---|
| **Windows `.dll`** | native MSVC: `cmake -B build -A Win32 && cmake --build build --config Release` (OpenSSL via vcpkg) | must be **MSVC ABI** — mingw loads but crashes omp-server |
| **Linux `.so`** | `ARCH=32 ./scripts/build-linux.sh` | i386 Debian **bullseye** (glibc 2.31) container; needs Docker |
| **macOS `.dylib`** | `cmake -B build && cmake --build build` | **dev only** — there is no macOS open.mp server, never shipped |

CI (`.github/workflows/build.yml`) builds and attaches the Windows + Linux artifacts
on tagged releases. Step-by-step per-OS instructions (including the Windows OpenSSL/vcpkg
setup and the macOS dev workflow) are in the
**[Building guide](https://github.com/Mac-Andreas/omp-MySQL/wiki/Building)**.

---

## Security model (in brief)

- **TLS is not optional** — the component refuses to open an unencrypted connection.
- **Prepared statements** bind parameters; the `%e` format specifier escapes the rest.
- **Argon2id** for passwords — slow-by-design, salted, on a worker thread.
- **Credentials** belong in the environment (`${DB_PASS}`) or a least-privilege DB user —
  see [tools/least-privilege-user.sql](tools/least-privilege-user.sql). Config obfuscation
  is available but honestly labeled (the key is operator-supplied, not in source).

Full threat model + hardening checklist:
[Security](https://github.com/Mac-Andreas/omp-MySQL/wiki/Security).

### Pentest & audit — v1.0.0

Each release documents what was actually tested. For **v1.0.0** the following were
exercised and **passed** (live, on the macOS/Linux/Windows builds):

| Area | What was tested | Result |
|---|---|---|
| SQL injection — login box | `' OR 1=1 --`, `admin'--`, quote payloads in the password field | ✅ inert (password is only hashed + compared, never in SQL) |
| SQL injection — registration | crafted names / payloads through the INSERT path | ✅ inert (bound prepared statement) |
| SQL injection — command args | payloads via `/unban`, `/acmds <x>`, etc. | ✅ inert (`%e` escaping / integer-only args) |
| Auth bypass — rejoin | log in → quit → rejoin on the same slot | ✅ password required again (no auto-login) |
| Auth bypass — slot reuse | imposter joining a timed-out player's slot | ✅ session wiped on connect; never inherited |
| Auth persistence | ESC/cancel the login dialog, idle past timeout | ✅ frozen until login; cancel/timeout = kicked |
| Re-auth replay | re-trigger login/register while already logged in | ✅ ignored (no `/login` command; guarded) |
| Privilege | first-account-becomes-owner | ✅ removed — admins only via RCON `/setlevel` |
| Memory safety | AddressSanitizer + UBSan run; static leak/UAF/thread audit | ✅ clean (0 errors) |
| Transport | downgrade to plaintext, self-signed cert | ✅ fail-closed; refuses unencrypted links |

> **Future versions:** I'll keep pentesting and will list here exactly what each
> release was tested against. If you find something this list doesn't cover, please
> open an issue — the goal is an honest, growing record, not a "trust me" badge.

---

## Documentation

The **[Wiki](https://github.com/Mac-Andreas/omp-MySQL/wiki)** is the book:

- **What is MySQL? / How a server talks to a database** (start-from-zero, with diagrams)
- **Getting started** · **Building** (Windows / Linux / macOS) · **Configuration**
- **Native reference** (every `mysql_*` function) · **Prepared statements** · **Models**
- **Migrating from SA-MP-MySQL** · **MySQL version history & support**
- **The omp-admin demo** · **Security**

---

## Credits

- **This component:** Xyranaut (Mac Andreas).
- **API inspiration:** SA-MP-MySQL by BlueG / maddinat0r — *no code reused*.
- **Dependencies:** MariaDB Connector/C (LGPL-2.1), OpenSSL, phc-winner-argon2,
  the open.mp SDK & Pawn toolchain. See [NOTICE.md](NOTICE.md).
