/* =========================================
 *
 *  MySQL for open.mp  —  connection wrapper impl
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#include "Connection.hpp"

#include <mysql.h>

// Cast the opaque handle stored in the header back to the real MYSQL*.
#define MYSQL_H() (static_cast<MYSQL*>(mysql_))

Connection::~Connection()
{
	close();
}

void Connection::close()
{
	if (mysql_ != nullptr)
	{
		mysql_close(MYSQL_H());
		mysql_ = nullptr;
	}
	connected_ = false;
}

SSLMode Connection::effectiveSSLMode(const ConnectionOptions& o) const
{
	// A CA always means we verify it. Otherwise honour the request, but the
	// enum has no insecure values, so the floor is REQUIRED either way.
	if (!o.sslCA.empty() || !o.sslCAPath.empty())
	{
		return (o.sslMode == SSLMode::VerifyIdentity) ? SSLMode::VerifyIdentity
													  : SSLMode::VerifyCA;
	}
	return o.sslMode;
}

bool Connection::open(const ConnectionOptions& o)
{
	close();

	mysql_ = mysql_init(nullptr);
	if (mysql_ == nullptr)
	{
		return false;
	}
	MYSQL* m = MYSQL_H();

	// --- Timeouts (no indefinite hangs) ---
	if (o.connectTimeout > 0)
	{
		unsigned int t = o.connectTimeout;
		mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &t);
	}
	if (o.readTimeout > 0)
	{
		unsigned int t = o.readTimeout;
		mysql_options(m, MYSQL_OPT_READ_TIMEOUT, &t);
	}
	if (o.writeTimeout > 0)
	{
		unsigned int t = o.writeTimeout;
		mysql_options(m, MYSQL_OPT_WRITE_TIMEOUT, &t);
	}

	// --- Charset: utf8mb4 only ---
	mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");

	// --- TLS: mandatory, modern, fail-closed -----------------------------
	// Pin protocols first so a downgrade can't happen. 1.0/1.1 are never set.
	if (!o.tlsVersion.empty())
	{
		mysql_options(m, MYSQL_OPT_TLS_VERSION, o.tlsVersion.c_str());
	}

	// Certificates / key / revocation, applied before the SSL mode.
	if (!o.sslKey.empty())
	{
		mysql_options(m, MYSQL_OPT_SSL_KEY, o.sslKey.c_str());
	}
	if (!o.sslCert.empty())
	{
		mysql_options(m, MYSQL_OPT_SSL_CERT, o.sslCert.c_str());
	}
	if (!o.sslCA.empty())
	{
		mysql_options(m, MYSQL_OPT_SSL_CA, o.sslCA.c_str());
	}
	if (!o.sslCAPath.empty())
	{
		mysql_options(m, MYSQL_OPT_SSL_CAPATH, o.sslCAPath.c_str());
	}
	if (!o.sslCipher.empty())
	{
		mysql_options(m, MYSQL_OPT_SSL_CIPHER, o.sslCipher.c_str());
	}
#if defined(MYSQL_OPT_SSL_CRL)
	if (!o.sslCRL.empty())
	{
		mysql_options(m, MYSQL_OPT_SSL_CRL, o.sslCRL.c_str());
	}
	if (!o.sslCRLPath.empty())
	{
		mysql_options(m, MYSQL_OPT_SSL_CRLPATH, o.sslCRLPath.c_str());
	}
#endif

	// Enforce encryption. This is the security keystone: the only modes we ever
	// request are REQUIRED or stricter, so the server can never downgrade us to
	// plaintext — the connect fails instead.
	//
	// Some MySQL clients expose MYSQL_OPT_SSL_MODE (REQUIRED / VERIFY_CA /
	// VERIFY_IDENTITY). MariaDB Connector/C (what we build from source) does NOT;
	// it expresses the same policy with two booleans: MYSQL_OPT_SSL_ENFORCE
	// (mandatory TLS) and MYSQL_OPT_SSL_VERIFY_SERVER_CERT (verify the chain).
	// Hostname verification (VERIFY_IDENTITY) maps to verify-server-cert too —
	// MariaDB verifies the hostname as part of server-cert verification.
	const SSLMode mode = effectiveSSLMode(o);
#if defined(MYSQL_OPT_SSL_MODE)
	{
		unsigned int sslMode = SSL_MODE_REQUIRED;
		switch (mode)
		{
		case SSLMode::VerifyCA: sslMode = SSL_MODE_VERIFY_CA; break;
		case SSLMode::VerifyIdentity: sslMode = SSL_MODE_VERIFY_IDENTITY; break;
		case SSLMode::Required:
		default: sslMode = SSL_MODE_REQUIRED; break;
		}
		mysql_options(m, MYSQL_OPT_SSL_MODE, &sslMode);
	}
#else
	{
		// MariaDB Connector/C path.
		my_bool enforce = 1; // TLS mandatory (REQUIRED and stricter)
		mysql_options(m, MYSQL_OPT_SSL_ENFORCE, &enforce);
		my_bool verify =
			(mode == SSLMode::VerifyCA || mode == SSLMode::VerifyIdentity) ? 1 : 0;
		mysql_options(m, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);
	}
#endif

	// --- Compression ---
	if (!o.compression.empty())
	{
#if defined(MYSQL_OPT_COMPRESSION_ALGORITHMS)
		mysql_options(m, MYSQL_OPT_COMPRESSION_ALGORITHMS, o.compression.c_str());
		if (o.zstdLevel > 0)
		{
			unsigned int lvl = static_cast<unsigned int>(o.zstdLevel);
			mysql_options(m, MYSQL_OPT_ZSTD_COMPRESSION_LEVEL, &lvl);
		}
#endif
	}

	unsigned long flags = 0;
	if (o.multiStatements)
	{
		flags |= CLIENT_MULTI_STATEMENTS;
	}

	MYSQL* ok = mysql_real_connect(
		m,
		o.host.c_str(),
		o.user.c_str(),
		o.password.c_str(),
		o.database.empty() ? nullptr : o.database.c_str(),
		o.port,
		nullptr, // unix socket
		flags);

	if (ok == nullptr)
	{
		connected_ = false;
		return false;
	}

	connected_ = true;
	return true;
}

bool Connection::query(const std::string& sql)
{
	if (!isOpen())
	{
		return false;
	}
	if (mysql_real_query(MYSQL_H(), sql.c_str(), static_cast<unsigned long>(sql.size())) != 0)
	{
		return false;
	}
	// Consume any result sets so the connection is left clean for reuse.
	MYSQL* m = MYSQL_H();
	do
	{
		MYSQL_RES* res = mysql_store_result(m);
		if (res != nullptr)
		{
			mysql_free_result(res);
		}
	} while (mysql_next_result(m) == 0);
	return true;
}

bool Connection::queryBuffered(const std::string& sql, QueryResult& out)
{
	out.sets.clear();
	out.queryString = sql;

	if (!isOpen())
	{
		return false;
	}

	MYSQL* m = MYSQL_H();
	if (mysql_real_query(m, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0)
	{
		return false;
	}

	// Walk every result set (multi-statement queries produce more than one).
	int status = 0;
	do
	{
		MYSQL_RES* res = mysql_store_result(m);

		ResultSet set;
		set.affectedRows = mysql_affected_rows(m);
		set.insertId = mysql_insert_id(m);
		set.warningCount = mysql_warning_count(m);

		if (res != nullptr)
		{
			unsigned int nfields = mysql_num_fields(res);
			MYSQL_FIELD* fields = mysql_fetch_fields(res);
			set.fields.reserve(nfields);
			for (unsigned int i = 0; i < nfields; ++i)
			{
				ResultField f;
				f.name = fields[i].name ? fields[i].name : "";
				f.type = static_cast<int>(fields[i].type);
				set.fields.push_back(std::move(f));
			}

			MYSQL_ROW row;
			while ((row = mysql_fetch_row(res)) != nullptr)
			{
				unsigned long* lengths = mysql_fetch_lengths(res);
				std::vector<ResultValue> cells;
				cells.reserve(nfields);
				for (unsigned int c = 0; c < nfields; ++c)
				{
					ResultValue v;
					if (row[c] == nullptr)
					{
						v.isNull = true;
					}
					else
					{
						v.value.assign(row[c], lengths ? lengths[c] : 0);
					}
					cells.push_back(std::move(v));
				}
				set.rows.push_back(std::move(cells));
			}

			mysql_free_result(res);
		}

		out.sets.push_back(std::move(set));

		status = mysql_next_result(m);
		// status: 0 = more results, -1 = no more, >0 = error on the next stmt.
	} while (status == 0);

	return status <= 0; // -1 (clean end) or 0 (shouldn't exit loop) are success
}

unsigned int Connection::lastErrno() const
{
	return mysql_ != nullptr ? mysql_errno(MYSQL_H()) : 0;
}

std::string Connection::lastError() const
{
	if (mysql_ == nullptr)
	{
		return "no connection handle";
	}
	const char* e = mysql_error(MYSQL_H());
	return e != nullptr ? std::string(e) : std::string();
}

std::string Connection::escape(const std::string& in) const
{
	if (mysql_ == nullptr)
	{
		return in;
	}
	// Worst case each byte becomes two, plus a NUL terminator.
	std::string out(in.size() * 2 + 1, '\0');
	unsigned long n = mysql_real_escape_string(
		MYSQL_H(), &out[0], in.c_str(), static_cast<unsigned long>(in.size()));
	out.resize(n);
	return out;
}

bool Connection::setCharset(const std::string& charset)
{
	if (mysql_ == nullptr)
	{
		return false;
	}
	return mysql_set_character_set(MYSQL_H(), charset.c_str()) == 0;
}

std::string Connection::getCharset() const
{
	if (mysql_ == nullptr)
	{
		return {};
	}
	const char* c = mysql_character_set_name(MYSQL_H());
	return c != nullptr ? std::string(c) : std::string();
}

std::string Connection::stat() const
{
	if (mysql_ == nullptr)
	{
		return {};
	}
	const char* s = mysql_stat(MYSQL_H());
	return s != nullptr ? std::string(s) : std::string();
}

std::string Connection::sslCipher() const
{
	if (mysql_ == nullptr)
	{
		return {};
	}
	// Fast path: the client-side cipher. This is populated on OpenSSL-backed
	// clients but can be empty on others (e.g. MariaDB Connector/C over Schannel)
	// even when the link IS encrypted.
	const char* c = mysql_get_ssl_cipher(MYSQL_H());
	if (c != nullptr && c[0] != '\0')
	{
		return std::string(c);
	}

	// Authoritative fallback: ask the SERVER for this session's TLS cipher. This
	// works regardless of the client TLS library, so the fail-closed encryption
	// check never produces a false negative. A non-empty value here proves the
	// connection is actually encrypted.
	MYSQL* m = MYSQL_H();
	if (mysql_real_query(m, "SHOW SESSION STATUS LIKE 'Ssl_cipher'", 36) != 0)
	{
		return {};
	}
	std::string cipher;
	if (MYSQL_RES* res = mysql_store_result(m))
	{
		if (MYSQL_ROW row = mysql_fetch_row(res))
		{
			// columns: [0]=Variable_name, [1]=Value (the cipher, "" if none).
			if (row[1] != nullptr && row[1][0] != '\0')
			{
				cipher = row[1];
			}
		}
		mysql_free_result(res);
	}
	return cipher;
}
