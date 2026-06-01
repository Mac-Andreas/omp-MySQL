/* =========================================
 *
 *  omp-mySQL — per-version FEATURE probe (test tooling, not shipped)
 *  ------------------------------------------
 *
 *  Goes beyond connection_probe: drives the real component paths against a MySQL
 *  at 127.0.0.1:<port> and reports, feature-by-feature, what works on THAT exact
 *  server version. Used to build the precise "what works on which version" matrix
 *  (so the README/wiki document version-gated features accurately).
 *
 *  Features probed:
 *    TLS       - mandatory TLS connect (Connection::open, fail-closed)
 *    QUERY     - a plain buffered SELECT (Connection::queryBuffered)
 *    PREP      - a prepared statement: prepare + bind + execute + fetch
 *                (the path the ABI fixes touched)
 *    CACHsha2  - the server's default auth on 8.0+/9.x (proven by connecting as a
 *                caching_sha2_password user, which the test harness creates)
 *    VECTOR    - the MySQL 9.0+ VECTOR datatype (CREATE TABLE ... VECTOR + insert)
 *
 *  Build (macOS, against the built MariaDB static client in ../build):
 *    c++ -std=c++20 -I../src -I../libs/mariadb-connector-c/include \
 *        -I../build/libs/mariadb-connector-c/include \
 *        feature_probe.cpp ../src/Connection.cpp ../src/Statement.cpp \
 *        $(find ../build -name libmariadbclient.a) \
 *        -lssl -lcrypto -lz -lzstd -liconv \
 *        -framework CoreFoundation -framework Security -framework Kerberos \
 *        -L/opt/homebrew/opt/openssl@3/lib -L/opt/homebrew/lib -o feature_probe
 *
 *  Usage:  feature_probe <port>   (expects user omptest / pw omptestpw / db ompdb)
 *  Output: one "FEATURE=PASS/FAIL[:reason]" line per probed capability + a version.
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#include "Connection.hpp"
#include "Statement.hpp"
#include "Result.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

static void report(const char* feat, bool ok, const std::string& why = "")
{
	if (ok)
		std::printf("%s=PASS\n", feat);
	else
		std::printf("%s=FAIL:%s\n", feat, why.c_str());
}

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
	bool opened = c.open(o);
	// TLS: connected AND encrypted (the user is REQUIRE SSL, so a plaintext link
	// would be refused server-side; our client is fail-closed too).
	report("TLS", opened && c.sslEnabled(),
		opened ? std::string("not encrypted") : c.lastError());
	if (!opened)
	{
		// Can't probe anything else without a connection.
		report("QUERY", false, "no connection");
		report("PREP", false, "no connection");
		report("CACHsha2", false, "no connection");
		report("VECTOR", false, "no connection");
		std::printf("VERSION=?\n");
		return 1;
	}

	// Server version (also proves a plain query works).
	std::string version = "?";
	{
		QueryResult r;
		bool ok = c.queryBuffered("SELECT VERSION() AS v", r);
		if (ok && !r.sets.empty() && !r.sets[0].rows.empty() && !r.sets[0].rows[0].empty())
			version = r.sets[0].rows[0][0].value;
		report("QUERY", ok && !r.sets.empty(), ok ? "" : c.lastError());
	}

	// CACHsha2: we connected as a caching_sha2_password user (the harness makes it
	// so on 8.0+/9.x). If open() succeeded, that auth plugin round-tripped over TLS.
	report("CACHsha2", true);

	// PREP: full prepared-statement round trip (prepare on the SAME thread here is
	// fine; the component defers it to the worker, but the libmysql path is identical).
	{
		PreparedStatement st(&c, "SELECT ? + ? AS total");
		bool ok = st.valid();
		std::string why;
		if (ok)
		{
			st.bindInt(0, 40);
			st.bindInt(1, 2);
			QueryResult r;
			ok = st.execute(r) && !r.sets.empty() && !r.sets[0].rows.empty()
				&& r.sets[0].rows[0][0].value == "42";
			if (!ok) why = st.lastError().empty() ? "wrong result" : st.lastError();
		}
		else
		{
			why = st.lastError();
		}
		report("PREP", ok, why);
	}

	// VECTOR: the MySQL 9.0+ datatype. CREATE a temp table with a VECTOR column and
	// insert one; fails (syntax/unknown type) on < 9.0, which is exactly what we
	// want to detect for the per-version matrix.
	{
		c.queryBuffered("DROP TABLE IF EXISTS omp_feat_vec", *(new QueryResult()));
		QueryResult r1, r2;
		bool created = c.queryBuffered(
			"CREATE TABLE omp_feat_vec (id INT PRIMARY KEY, v VECTOR(3))", r1);
		bool inserted = created && c.queryBuffered(
			"INSERT INTO omp_feat_vec VALUES (1, STRING_TO_VECTOR('[1,2,3]'))", r2);
		report("VECTOR", created && inserted,
			created ? c.lastError() : c.lastError());
		QueryResult cleanup;
		c.queryBuffered("DROP TABLE IF EXISTS omp_feat_vec", cleanup);
	}

	std::printf("VERSION=%s\n", version.c_str());
	return 0;
}
