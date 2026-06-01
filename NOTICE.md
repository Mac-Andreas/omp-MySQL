# NOTICE

This project is an **independent, clean-room implementation** of a MySQL
database plugin for [open.mp](https://github.com/openmultiplayer/open.mp).

It is **inspired by** the classic
[SA-MP-MySQL](https://github.com/pBlueG/SA-MP-MySQL) plugin (by BlueG /
maddinat0r, BSD-3-Clause) in the sense that it reimplements the same *public
Pawn API* — native function names, callback names, and constant names — so that
existing gamemodes can compile against it with minimal changes.

**No source code from SA-MP-MySQL (or any other plugin) has been copied,
adapted, or transcribed into this project.** Every C++ source file, the Pawn
include, the build scripts, and the documentation are original work, written
from scratch against:

- the MySQL C API public documentation,
- the open.mp Component SDK,
- and the *observable* Pawn API surface (native/callback/constant names only).

This project's own code is released under the **MIT License** (see `LICENSE`).
It links the **MariaDB Connector/C**, which is **LGPL-2.1** — that license
applies to the connector only and does **not** make this project GPL/LGPL. Use
it anywhere, including in closed-source gamemodes.

Because the connector is statically linked, LGPL-2.1 §6 requires that recipients
be able to relink the combined work against a modified connector. We satisfy
this by distributing the connector's complete corresponding source — it is the
pinned git submodule `libs/mariadb-connector-c` (MariaDB Connector/C 3.4.8) — so
anyone can rebuild it; the component's own object files can be provided on
request to enable relinking.

## Third-party components

- **open.mp SDK** — MIT — https://github.com/openmultiplayer/open.mp-sdk
- **pawn-natives** — https://github.com/openmultiplayer/pawn-natives
- **open.mp Pawn compiler headers** — https://github.com/openmultiplayer/compiler
- **MariaDB Connector/C 3.4.8** — **LGPL-2.1** —
  https://github.com/mariadb-corporation/mariadb-connector-c . Vendored as the
  git submodule `libs/mariadb-connector-c` and built from source (statically
  linked; modern TLS — OpenSSL on every platform).
- **Argon2** (phc-winner-argon2) — CC0/Apache-2.0 — vendored under `libs/argon2/`.

## Trademarks & affiliation

omp-MySQL is an independent, community-made project. It is **not affiliated with,
sponsored by, or endorsed by** the open.mp project or Oracle Corporation. **"MySQL"**
is a trademark of Oracle Corporation; **"open.mp"** / "open multiplayer" of its
respective authors; **"SA-MP"** of the SA-MP team. These names are used here only
descriptively (nominative use), to indicate compatibility and lineage — not to claim
any endorsement or ownership. "omp-MySQL" is this project's own name.

Author: **Xyranaut** · Organisation: **Mac Andreas**
