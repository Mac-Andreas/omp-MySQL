# testscripts/

Developer test tooling — **not shipped** with the component.

## MySQL version-compatibility test

Verifies the component can open a real, TLS-mandatory connection to each MySQL
server version. Drives the actual `Connection::open()` (not a mock).

- `connection_probe.cpp` — native probe; connects to `127.0.0.1:<port>` as
  `omptest`/`omptestpw` and prints the result + negotiated TLS cipher.
- `test_mysql_versions.sh` — spins up each MySQL version in Docker, creates the
  test user, runs the probe, and records PASS/FAIL.

### Run it

```sh
# 1. build the probe (macOS; needs the component already built in ../build)
cd testscripts
c++ -std=c++20 -I../src -I../libs/mariadb-connector-c/include \
    -I../build/libs/mariadb-connector-c/include \
    connection_probe.cpp ../src/Connection.cpp \
    $(find ../build -name libmariadbclient.a) \
    -lssl -lcrypto -lz -lzstd -liconv \
    -framework CoreFoundation -framework Security -framework Kerberos \
    -L/opt/homebrew/opt/openssl@3/lib -L/opt/homebrew/lib -o connection_probe
cp connection_probe /tmp/ver_test   # the runner looks for /tmp/ver_test

# 2. run across versions (needs Docker / colima)
./test_mysql_versions.sh 9.4 9.3 9.2 9.1 9.0 8.4 8.3 8.2 8.1 8.0 5.7
cat /tmp/ver_results.txt
```

### Last results (2026-05-31)

Min supported = **5.7** (TLS 1.2); 8.0+ negotiate TLS 1.3; **8.4 LTS
recommended**. 5.6/5.5 fail because their default server has no TLS
(`have_ssl = DISABLED`) and the component is fail-closed. See the full support
table in the top-level `README.md`.

| Server | Result | TLS / reason |
|---|---|---|
| 9.4.0 … 9.0.1 | PASS | TLS 1.3 |
| 8.4.9 … 8.0.46 | PASS | TLS 1.3 |
| 5.7.44 | PASS | TLS 1.2 |
| 5.6.51, 5.5.62 | FAIL | server `have_ssl=DISABLED` (no TLS) → fail-closed refuses |
