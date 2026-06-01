#!/bin/bash
# Spin each MySQL version in Docker, provision a TLS caching_sha2 (or native for
# 5.x) user, and run feature_probe to record per-version feature support.
set -u
VERSIONS="${*:-9.2 9.1 9.0 8.4 8.0 5.7 5.6}"
PORT=3399
OUT=feature-matrix.txt
: > "$OUT"
for V in $VERSIONS; do
  NAME="ompfeat_${V//./_}"
  docker rm -f "$NAME" >/dev/null 2>&1
  echo ">>> $V : starting container (port $PORT)" >&2
  docker run -d --name "$NAME" --platform linux/amd64 \
    -e MYSQL_ROOT_PASSWORD=rootpw -e MYSQL_DATABASE=ompdb \
    -p "$PORT:3306" "mysql:$V" >/dev/null 2>&1
  rdy=0
  for i in $(seq 1 90); do
    docker exec "$NAME" mysql -uroot -prootpw -N -e "SELECT 1" >/dev/null 2>&1 && { rdy=1; break; }
    sleep 2
  done
  if [ "$rdy" = 0 ]; then echo "$V | SERVER-NEVER-READY" >>"$OUT"; docker rm -f "$NAME">/dev/null 2>&1; PORT=$((PORT+1)); continue; fi
  # user: caching_sha2 (8.0+/9.x) falls back to native (5.x)
  docker exec "$NAME" mysql -uroot -prootpw -e "
    CREATE DATABASE IF NOT EXISTS ompdb;
    CREATE USER IF NOT EXISTS 'omptest'@'%' IDENTIFIED WITH caching_sha2_password BY 'omptestpw' REQUIRE SSL;
    GRANT ALL ON ompdb.* TO 'omptest'@'%'; FLUSH PRIVILEGES;" 2>/dev/null \
  || docker exec "$NAME" mysql -uroot -prootpw -e "
    CREATE DATABASE IF NOT EXISTS ompdb;
    CREATE USER IF NOT EXISTS 'omptest'@'%' IDENTIFIED WITH mysql_native_password BY 'omptestpw' REQUIRE SSL;
    GRANT ALL ON ompdb.* TO 'omptest'@'%'; FLUSH PRIVILEGES;" 2>/dev/null
  sleep 2
  RES=$(./feature_probe "$PORT" 2>&1 | tr '\n' ' ')
  echo "$V | $RES" >>"$OUT"
  echo ">>> $V : $RES" >&2
  docker rm -f "$NAME" >/dev/null 2>&1
  PORT=$((PORT+1))
done
echo "=== MATRIX ===" >&2
cat "$OUT" >&2
