#!/usr/bin/env bash
#
# omp-mySQL — MySQL version-compatibility tester (builds the support matrix).
#
# For each MySQL version given, starts a Docker container with TLS, creates an
# `omptest` user (REQUIRE SSL), and runs the native ./connection_probe against
# it, recording PASS/FAIL + server version + negotiated TLS cipher to
# /tmp/ver_results.txt. Requires Docker (colima) and a built ./connection_probe.
#
# Usage:  ./test_mysql_versions.sh 9.4 9.3 9.2 9.1 9.0 8.4 8.3 8.2 8.1 8.0 5.7
#
# Notes: readiness is checked with a real root TCP query (not `mysqladmin ping`,
# which reports alive even on auth failure); the test user is created as root
# over TCP (-h127.0.0.1); native_password is tried first, caching_sha2 as fallback.
RESULT=/tmp/ver_results.txt; : > "$RESULT"
for V in "$@"; do
  NAME="omptest-mysql-$V"; PORT=$((3520 + RANDOM % 300))
  docker rm -f "$NAME" >/dev/null 2>&1
  echo ">>> $V on :$PORT" >&2
  docker run -d --name "$NAME" --platform linux/amd64 -e MYSQL_ROOT_PASSWORD=rootpw -e MYSQL_DATABASE=ompdb -p "$PORT:3306" "mysql:$V" >/dev/null 2>&1
  rdy=0
  for i in $(seq 1 90); do docker exec "$NAME" mysql -h127.0.0.1 -uroot -prootpw -N -e "SELECT 1" >/dev/null 2>&1 && { rdy=1; break; }; sleep 2; done
  if [ "$rdy" = 0 ]; then echo "$V|FAIL|server never became ready" >>"$RESULT"; docker rm -f "$NAME">/dev/null 2>&1; continue; fi
  ERR=$(docker exec "$NAME" mysql -h127.0.0.1 -uroot -prootpw -e "CREATE USER IF NOT EXISTS 'omptest'@'%' IDENTIFIED WITH mysql_native_password BY 'omptestpw' REQUIRE SSL; GRANT ALL ON ompdb.* TO 'omptest'@'%'; FLUSH PRIVILEGES;" 2>&1 | grep -i error)
  AUTH="mysql_native_password"
  if [ -n "$ERR" ]; then
    docker exec "$NAME" mysql -h127.0.0.1 -uroot -prootpw -e "CREATE USER IF NOT EXISTS 'omptest'@'%' IDENTIFIED WITH caching_sha2_password BY 'omptestpw' REQUIRE SSL; GRANT ALL ON ompdb.* TO 'omptest'@'%'; FLUSH PRIVILEGES;" 2>/dev/null
    AUTH="caching_sha2_password"
  fi
  OUT=$(DYLD_LIBRARY_PATH=/opt/homebrew/opt/openssl@3/lib /tmp/ver_test "$PORT" 2>&1); RC=$?
  FULLV=$(docker exec "$NAME" mysql -h127.0.0.1 -uroot -prootpw -N -e "SELECT @@version" 2>/dev/null)
  CIPHER=$(echo "$OUT" | grep -oE 'CIPHER=[^ ]*' | cut -d= -f2)
  if [ "$RC" = 0 ]; then echo "$V|PASS|$FULLV|auth=$AUTH|cipher=$CIPHER" >>"$RESULT";
  else ERRM=$(echo "$OUT"|grep -oE 'ERR=.*'|cut -d= -f2-); echo "$V|FAIL|$FULLV|auth=$AUTH|${ERRM:-$OUT}" >>"$RESULT"; fi
  docker rm -f "$NAME" >/dev/null 2>&1
done
cat "$RESULT"
