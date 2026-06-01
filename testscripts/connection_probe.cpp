/* =========================================
 *
 *  omp-mySQL — native connection probe (test tooling, not shipped)
 *  ------------------------------------------
 *
 *  Drives the real Connection::open() against a MySQL at 127.0.0.1:<port> and
 *  prints whether the TLS-mandatory connect succeeded + the negotiated cipher.
 *  Used by test_mysql_versions.sh to build the version-support matrix.
 *
 *  Build (macOS, against the already-built MariaDB static client in ../build):
 *    c++ -std=c++20 -I../src -I../libs/mariadb-connector-c/include \
 *        -I../build/libs/mariadb-connector-c/include \
 *        connection_probe.cpp ../src/Connection.cpp \
 *        $(find ../build -name libmariadbclient.a) \
 *        -lssl -lcrypto -lz -lzstd -liconv \
 *        -framework CoreFoundation -framework Security -framework Kerberos \
 *        -L/opt/homebrew/opt/openssl@3/lib -L/opt/homebrew/lib -o connection_probe
 *
 *  Usage:  connection_probe <port>   (expects user omptest / pw omptestpw / db ompdb)
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#include "Connection.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv)
{
	int port = argc > 1 ? std::atoi(argv[1]) : 3306;
	ConnectionOptions o;
	o.host = "127.0.0.1";
	o.port = static_cast<unsigned int>(port);
	o.user = "omptest";
	o.password = "omptestpw";
	o.database = "ompdb";
	o.sslMode = SSLMode::Required;

	Connection c;
	bool ok = c.open(o);
	std::printf("PORT=%d OPEN=%d ERRNO=%u CIPHER=%s SSL=%d ERR=%s\n",
		port, ok ? 1 : 0, c.lastErrno(), c.sslCipher().c_str(),
		c.sslEnabled() ? 1 : 0, c.lastError().c_str());
	return ok ? 0 : 1;
}
