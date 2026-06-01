/* =========================================
 *
 *  MySQL for open.mp  —  connection wrapper
 *  ------------------------------------------
 *
 *  Owns a single MYSQL* and enforces the project's security policy:
 *  connections are ALWAYS TLS-encrypted and fail closed. There is no plaintext
 *  code path. Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#pragma once

#include "Result.hpp"

#include <string>
#include <cstdint>

// TLS verification strictness — mirrors include/omp-mysql.inc E_MYSQL_SSL_MODE.
// All values are encrypted; only DISABLED/PREFERRED (insecure) are absent.
enum class SSLMode : int
{
	Required = 0,       // encrypt; don't verify the certificate
	VerifyCA = 1,       // encrypt + verify the cert chains to a trusted CA
	VerifyIdentity = 2, // encrypt + verify CA + server hostname
};

// Everything needed to open a connection. Defaults are secure.
struct ConnectionOptions
{
	std::string host = "127.0.0.1";
	std::string user;
	std::string password;
	std::string database;
	unsigned int port = 3306;

	unsigned int connectTimeout = 0; // seconds (0 = client default)
	unsigned int readTimeout = 0;
	unsigned int writeTimeout = 0;

	bool autoReconnect = false;
	bool multiStatements = false; // OFF by default (injection hardening)

	// --- TLS (no "off" switch by design) ---
	SSLMode sslMode = SSLMode::VerifyCA; // effective default resolved at connect
	std::string sslCA;
	std::string sslCAPath;
	std::string sslCert;
	std::string sslKey;
	std::string sslCRL;
	std::string sslCRLPath;
	std::string sslCipher;
	std::string tlsVersion = "TLSv1.3,TLSv1.2"; // 1.0/1.1 never allowed

	// --- Compression ---
	std::string compression; // "", "zstd", "zlib", "zstd,zlib"
	int zstdLevel = 3;
};

// The underlying handle is kept as an opaque pointer so this header doesn't
// drag <mysql.h> into every includer; Connection.cpp casts it to MYSQL*.
class Connection
{
public:
	Connection() = default;
	~Connection();

	Connection(const Connection&) = delete;
	Connection& operator=(const Connection&) = delete;

	/// Open the connection with the given options. Applies TLS fail-closed
	/// (mandatory encryption) before connecting. Returns true on success; on
	/// failure use lastErrno()/lastError().
	bool open(const ConnectionOptions& options);

	/// Close and release the underlying handle (idempotent).
	void close();

	bool isOpen() const { return mysql_ != nullptr && connected_; }

	/// Run a query synchronously, discarding any result rows. Returns true on
	/// success. Use queryBuffered() when you need the rows.
	bool query(const std::string& sql);

	/// Run a query and fully buffer every result set into `out` (rows copied so
	/// the connection is free to move on). Handles multi-statement results.
	/// Returns true on success; on failure use lastErrno()/lastError().
	bool queryBuffered(const std::string& sql, QueryResult& out);

	unsigned int lastErrno() const;
	std::string lastError() const;

	/// Escape `in` for safe inclusion inside single quotes in a query, honouring
	/// the connection's charset (uses mysql_real_escape_string).
	std::string escape(const std::string& in) const;

	/// Set / get the connection character set.
	bool setCharset(const std::string& charset);
	std::string getCharset() const;

	/// Server status string (as `mysqladmin status` shows).
	std::string stat() const;

	/// Negotiated TLS cipher (empty if, somehow, not encrypted).
	std::string sslCipher() const;

	/// True if the live connection is encrypted (true by construction here).
	bool sslEnabled() const { return isOpen() && !sslCipher().empty(); }

	/// Opaque MYSQL* (cast in the .cpp). Null until open() succeeds.
	void* raw() const { return mysql_; }

private:
	void* mysql_ = nullptr; // MYSQL*
	bool connected_ = false;

	// Resolve the effective SSL mode: a CA implies VERIFY_CA, otherwise the
	// caller's choice, but never below REQUIRED.
	SSLMode effectiveSSLMode(const ConnectionOptions& o) const;
};
