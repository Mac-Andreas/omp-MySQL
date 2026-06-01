/* =========================================
 *
 *  MySQL for open.mp  —  native API
 *  ------------------------------------------
 *
 *  Phase 2 subset: connection management, error reporting, TLS introspection,
 *  and a synchronous query. Async / cache / prepared statements / hashing /
 *  vectors arrive in later phases.
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

// pawn_natives.hpp here for SCRIPT_API; pawn_impl.hpp is included in exactly
// ONE translation unit (MySQLComponent.cpp) to avoid duplicate amx_* symbols.
#include <Server/Components/Pawn/Impl/pawn_natives.hpp>

#include "MySQLComponent.hpp"
#include "Connection.hpp"
#include "Statement.hpp"
#include "Hashing.hpp"

#include "Query.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

static MySQLComponent* mysqlc()
{
	return MySQLComponent::getInstance();
}

// Ordinals of E_MYSQL_OPTION in include/omp-mysql.inc (keep in sync with the enum).
enum MySQLOptionType
{
	OPT_AUTO_RECONNECT = 0,
	OPT_MULTI_STATEMENTS,
	OPT_POOL_SIZE,
	OPT_SERVER_PORT,
	OPT_CONNECT_TIMEOUT,
	OPT_READ_TIMEOUT,
	OPT_WRITE_TIMEOUT,
	OPT_SSL_MODE,
	OPT_SSL_CA,
	OPT_SSL_CAPATH,
	OPT_SSL_CERT,
	OPT_SSL_KEY,
	OPT_SSL_CRL,
	OPT_SSL_CRLPATH,
	OPT_SSL_CIPHER,
	OPT_TLS_VERSION,
	OPT_COMPRESSION,
	OPT_ZSTD_COMPRESSION_LEVEL,
	OPT_MAX_QUERIES_PER_SECOND,
	OPT_MAX_QUERY_LENGTH,
	OPT_MAX_PENDING_QUERIES,
};

// Expand ${VAR} occurrences from the environment so secrets (passwords, key
// paths) can live outside the .ini / source. Unknown vars expand to empty.
static std::string ExpandEnv(const std::string& in)
{
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size();)
	{
		if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{')
		{
			size_t end = in.find('}', i + 2);
			if (end != std::string::npos)
			{
				std::string name = in.substr(i + 2, end - (i + 2));
				const char* val = name.empty() ? nullptr : std::getenv(name.c_str());
				if (val != nullptr)
				{
					out += val;
				}
				i = end + 1;
				continue;
			}
		}
		out += in[i++];
	}
	return out;
}

// --- Config-file obfuscation ------------------------------------------------
// An OPTIONAL light obfuscation for the connect config, so a casually dumped
// server directory doesn't expose credentials in plaintext.
//
// HONEST LABEL: obfuscation, NOT encryption. The ALGORITHM (this XOR keystream)
// is public source. The only thing that keeps a blob unreadable is the KEY — and
// because client-side crypto can never hide a key from whoever runs the binary,
// we don't pretend to: the key is an OPERATOR SECRET supplied via the OMP_CFG_KEY
// environment variable, exactly like the DB password. The public source shows the
// method, never the key; a dumped server directory yields the blob but not the
// env-only key, so it can't be decoded from the files alone. If unset, a clearly
// weak built-in default is used (and a warning logged) — that case is barely more
// than base64. Real protection is ${ENV} secrets + a least-privilege DB user +
// mandatory TLS; this is only a thin "not plaintext on disk" layer on top.
//
// Files begin with the magic "OMPMENC1"; mysql_config_obfuscate() writes them and
// mysql_connect_config() decodes them transparently.
static const char kCfgMagic[] = "OMPMENC1";

// Derive the keystream seed from the operator's OMP_CFG_KEY (FNV-1a over the env
// value). Returns 0 in `fromEnv` when the env var is absent/empty so the caller
// can warn that only the weak default is protecting the blob.
static std::uint32_t CfgKeySeed(bool* fromEnv)
{
	const char* env = std::getenv("OMP_CFG_KEY");
	if (env != nullptr && env[0] != '\0')
	{
		std::uint32_t h = 2166136261u; // FNV-1a
		for (const char* p = env; *p; ++p)
		{
			h ^= static_cast<unsigned char>(*p);
			h *= 16777619u;
		}
		if (h == 0) h = 0x1u;
		if (fromEnv) *fromEnv = true;
		return h;
	}
	if (fromEnv) *fromEnv = false;
	return 0x6F6D704Du; // weak fallback ("ompM") — documented as ~base64 strength
}

static std::string CfgXor(const std::string& body)
{
	std::uint32_t s = CfgKeySeed(nullptr);
	std::string out;
	out.resize(body.size());
	for (size_t i = 0; i < body.size(); ++i)
	{
		// xorshift32 keystream byte.
		s ^= s << 13; s ^= s >> 17; s ^= s << 5;
		out[i] = static_cast<char>(static_cast<unsigned char>(body[i])
			^ static_cast<unsigned char>(s & 0xFF));
	}
	return out;
}

// If `raw` is an obfuscated config (magic header), return the decoded text;
// otherwise return it unchanged. XOR is symmetric, so the same routine encodes.
static std::string CfgDecodeIfNeeded(const std::string& raw)
{
	const size_t magicLen = sizeof(kCfgMagic) - 1;
	if (raw.size() >= magicLen && raw.compare(0, magicLen, kCfgMagic) == 0)
	{
		return CfgXor(raw.substr(magicLen));
	}
	return raw;
}

// ===========================================================================
//  Connection management
// ===========================================================================

// Shared connect path: open `opts`, enforce encryption, register + log.
// Returns the new handle or 0 (MYSQL_INVALID_HANDLE).
static int DoConnect(ConnectionOptions opts)
{
	mysqlc()->debugLog(StringView("connect: " + opts.user + "@" + opts.host + ":"
		+ std::to_string(opts.port) + "/" + opts.database
		+ " (TLS mandatory, ssl_mode=" + std::to_string(static_cast<int>(opts.sslMode)) + ")"));
	auto conn = std::make_unique<Connection>();
	if (!conn->open(opts))
	{
		std::string err = conn->lastError();
		mysqlc()->log(LogLevel::Error, StringView(err));
		mysqlc()->debugLog(StringView("connect FAILED: " + err));
		return 0;
	}

	// Confirm encryption and log the negotiated cipher (operator visibility).
	std::string cipher = conn->sslCipher();
	if (cipher.empty())
	{
		mysqlc()->log(LogLevel::Error, "connection is not encrypted; refusing it");
		return 0;
	}
	std::string ok = "connected (TLS " + cipher + ")";
	mysqlc()->log(LogLevel::Message, StringView(ok));

	int handle = mysqlc()->addConnection(std::move(conn), opts.multiStatements);
	mysqlc()->debugLog(StringView("connect OK: handle=" + std::to_string(handle)
		+ " cipher=" + cipher));
	return handle;
}

// MySQLConfig:mysql_config_create()
SCRIPT_API(mysql_config_create, int())
{
	return mysqlc()->addOptions();
}

// True for the option types whose value is a string (the rest are integers).
static bool OptionIsString(int type)
{
	switch (type)
	{
	case OPT_SSL_CA:
	case OPT_SSL_CAPATH:
	case OPT_SSL_CERT:
	case OPT_SSL_KEY:
	case OPT_SSL_CRL:
	case OPT_SSL_CRLPATH:
	case OPT_SSL_CIPHER:
	case OPT_TLS_VERSION:
	case OPT_COMPRESSION:
		return true;
	default:
		return false;
	}
}

static void ApplyIntOption(ConnectionOptions* o, int type, int value)
{
	switch (type)
	{
	case OPT_AUTO_RECONNECT: o->autoReconnect = value != 0; break;
	case OPT_MULTI_STATEMENTS: o->multiStatements = value != 0; break;
	case OPT_SERVER_PORT: o->port = static_cast<unsigned int>(value); break;
	case OPT_CONNECT_TIMEOUT: o->connectTimeout = static_cast<unsigned int>(value); break;
	case OPT_READ_TIMEOUT: o->readTimeout = static_cast<unsigned int>(value); break;
	case OPT_WRITE_TIMEOUT: o->writeTimeout = static_cast<unsigned int>(value); break;
	case OPT_SSL_MODE:
		// Clamp to the secure range; out-of-range leaves the default untouched.
		if (value >= 0 && value <= 2)
		{
			o->sslMode = static_cast<SSLMode>(value);
		}
		break;
	case OPT_ZSTD_COMPRESSION_LEVEL: o->zstdLevel = value; break;
	default: break;
	}
}

static void ApplyStringOption(ConnectionOptions* o, int type, const std::string& value)
{
	switch (type)
	{
	case OPT_SSL_CA: o->sslCA = value; break;
	case OPT_SSL_CAPATH: o->sslCAPath = value; break;
	case OPT_SSL_CERT: o->sslCert = value; break;
	case OPT_SSL_KEY: o->sslKey = value; break;
	case OPT_SSL_CRL: o->sslCRL = value; break;
	case OPT_SSL_CRLPATH: o->sslCRLPath = value; break;
	case OPT_SSL_CIPHER: o->sslCipher = value; break;
	case OPT_TLS_VERSION: o->tlsVersion = value; break;
	case OPT_COMPRESSION: o->compression = value; break;
	default: break;
	}
}

// mysql_config_set(MySQLConfig:options, E_MYSQL_OPTION:type, value-or-string)
//
// Registered as a classic AMX native (see MySQL_RegisterClassicNatives) because
// the single value parameter may be an integer or a string; we pick by the
// option type. AMX convention: params[0]=byte-count, params[1]=FIRST arg.
// params: [0]=count, [1]=options, [2]=type, [3]=value.
static cell AMX_NATIVE_CALL n_mysql_config_set(AMX* amx, const cell* params)
{
	int options = static_cast<int>(params[1]);
	int type = static_cast<int>(params[2]);

	ConnectionOptions* o = mysqlc()->getOptions(options);
	if (o == nullptr)
	{
		return 0;
	}

	if (OptionIsString(type))
	{
		cell* addr = nullptr;
		if (amx_GetAddr(amx, params[3], &addr) != AMX_ERR_NONE || addr == nullptr)
		{
			return 0;
		}
		int len = 0;
		amx_StrLen(addr, &len);
		std::string value(static_cast<size_t>(len), '\0');
		if (len > 0)
		{
			amx_GetString(&value[0], addr, 0, len + 1);
		}
		ApplyStringOption(o, type, value);
	}
	else
	{
		// The value is the variadic ('...') arg -> passed BY REFERENCE. Deref it;
		// reading params[3] directly would store the AMX address (the 222368 bug).
		cell* addr = nullptr;
		int value = 0;
		if (amx_GetAddr(amx, params[3], &addr) == AMX_ERR_NONE && addr != nullptr)
		{
			value = static_cast<int>(*addr);
		}
		ApplyIntOption(o, type, value);
	}
	return 1;
}

// Decode the printf-style `format` mini-language into pre-marshalled callback
// args (done on the MAIN thread; the worker never touches the AMX).
//   d / i -> integer    f -> float    s -> string
// `firstValue` is the 1-based params index of the first value cell.
static void DecodeFormat(AMX* amx, const cell* params, int firstValue,
	const std::string& fmt, std::vector<CallbackArg>& out)
{
	int paramCount = static_cast<int>(params[0] / static_cast<cell>(sizeof(cell)));
	int p = firstValue;
	for (char spec : fmt)
	{
		if (p > paramCount)
		{
			break; // ran out of supplied values
		}
		// Pawn passes EVERY variadic ('...') argument BY REFERENCE — params[p] is
		// the AMX address of the value, not the value. Deref via amx_GetAddr for
		// ints/floats too (not just strings); reading params[p] directly yields a
		// garbage address. (Matches the vendored amx printf impl in amxcons.c.)
		switch (spec)
		{
		case 'd':
		case 'i':
		{
			cell* addr = nullptr;
			int v = 0;
			if (amx_GetAddr(amx, params[p], &addr) == AMX_ERR_NONE && addr != nullptr)
			{
				v = static_cast<int>(*addr);
			}
			out.push_back(CallbackArg::makeInt(v));
			++p;
			break;
		}
		case 'f':
		{
			cell* addr = nullptr;
			float f = 0.0f;
			if (amx_GetAddr(amx, params[p], &addr) == AMX_ERR_NONE && addr != nullptr)
			{
				cell c = *addr;
				std::memcpy(&f, &c, sizeof(f));
			}
			out.push_back(CallbackArg::makeFloat(f));
			++p;
			break;
		}
		case 's':
		{
			cell* addr = nullptr;
			std::string s;
			if (amx_GetAddr(amx, params[p], &addr) == AMX_ERR_NONE && addr != nullptr)
			{
				int len = 0;
				amx_StrLen(addr, &len);
				s.resize(static_cast<size_t>(len), '\0');
				if (len > 0)
				{
					amx_GetString(&s[0], addr, 0, len + 1);
				}
			}
			out.push_back(CallbackArg::makeString(std::move(s)));
			++p;
			break;
		}
		default:
			// Unknown specifier: skip it (don't consume a value).
			break;
		}
	}
}

// Read the AMX string at params[idx] into a std::string.
static std::string AmxStringParam(AMX* amx, cell param)
{
	cell* addr = nullptr;
	std::string s;
	if (amx_GetAddr(amx, param, &addr) == AMX_ERR_NONE && addr != nullptr)
	{
		int len = 0;
		amx_StrLen(addr, &len);
		s.resize(static_cast<size_t>(len), '\0');
		if (len > 0)
		{
			amx_GetString(&s[0], addr, 0, len + 1);
		}
	}
	return s;
}

// Human-readable reason for a guard rejection (for logs / OnQueryError text).
static const char* GuardRejectText(MySQLComponent::GuardReject r)
{
	switch (r)
	{
	case MySQLComponent::GuardReject::UnknownHandle:  return "invalid connection handle";
	case MySQLComponent::GuardReject::TooLong:        return "query exceeds the configured length limit";
	case MySQLComponent::GuardReject::RateLimited:    return "query rate limit exceeded";
	case MySQLComponent::GuardReject::TooManyPending: return "too many pending queries";
	case MySQLComponent::GuardReject::MultiStatement: return "multiple statements rejected (CLIENT_MULTI_STATEMENTS not enabled)";
	default:                                          return "query rejected";
	}
}

// Fire OnQueryError(0, reason, callback, query, handle) on the main script so a
// guard-rejected query is observable, mirroring a failed execution. handle 0
// (UnknownHandle) is left silent — there's no meaningful handle to report on.
static void ReportGuardReject(AMX* amx, int handle, const std::string& sql,
	const std::string& callback, MySQLComponent::GuardReject reason)
{
	if (reason == MySQLComponent::GuardReject::UnknownHandle)
	{
		return;
	}
	int idx = -1;
	if (amx_FindPublic(amx, "OnQueryError", &idx) != AMX_ERR_NONE)
	{
		return;
	}
	const char* text = GuardRejectText(reason);
	cell amxQuery = 0, amxError = 0, amxCallback = 0;
	cell* phys = nullptr;
	amx_PushString(amx, &amxQuery, &phys, sql.c_str(), 0, 0);
	amx_PushString(amx, &amxError, &phys, text, 0, 0);
	amx_PushString(amx, &amxCallback, &phys, callback.c_str(), 0, 0);
	// REVERSE push: handle, query, callback, error, errorid(0).
	amx_Push(amx, static_cast<cell>(handle));
	amx_Push(amx, amxQuery);
	amx_Push(amx, amxCallback);
	amx_Push(amx, amxError);
	amx_Push(amx, 0);
	cell ret = 0;
	amx_Exec(amx, &ret, idx);
	amx_Release(amx, amxCallback);
	amx_Release(amx, amxError);
	amx_Release(amx, amxQuery);
}

// mysql_execute(MySQL:handle, query[], callback[]="", format[]="", {Float,_}:...)
// AMX convention: params[0]=arg byte-count, params[1]=FIRST arg.
// params: [0]=count [1]=handle [2]=query [3]=callback [4]=format [5+]=values
static cell AMX_NATIVE_CALL n_mysql_execute(AMX* amx, const cell* params)
{
	int paramCount = static_cast<int>(params[0] / static_cast<cell>(sizeof(cell)));
	if (paramCount < 2)
	{
		return 0;
	}

	auto job = std::make_unique<QueryJob>();
	job->connectionHandle = static_cast<int>(params[1]);
	job->sql = AmxStringParam(amx, params[2]);
	if (paramCount >= 3)
	{
		job->callback = AmxStringParam(amx, params[3]);
	}
	if (paramCount >= 4)
	{
		std::string fmt = AmxStringParam(amx, params[4]);
		DecodeFormat(amx, params, 5, fmt, job->args);
	}

	std::string sql = job->sql;
	std::string cb = job->callback;
	int handle = job->connectionHandle;
	MySQLComponent::GuardReject reason = MySQLComponent::GuardReject::None;
	if (mysqlc()->enqueueJob(std::move(job), &reason))
	{
		return 1;
	}
	ReportGuardReject(amx, handle, sql, cb, reason);
	return 0;
}

// mysql_execute_for(MySQL:handle, query[], callback[]="", format[]="", {Float,_}:...)
// Same as tquery, but bound to a player: the first integer-typed callback arg is
// treated as the playerid; if that player has left by the time results return,
// the callback is skipped (see MySQLComponent::dispatchJob).
static cell AMX_NATIVE_CALL n_mysql_execute_for(AMX* amx, const cell* params)
{
	int paramCount = static_cast<int>(params[0] / static_cast<cell>(sizeof(cell)));
	if (paramCount < 2)
	{
		return 0;
	}

	auto job = std::make_unique<QueryJob>();
	job->connectionHandle = static_cast<int>(params[1]);
	job->sql = AmxStringParam(amx, params[2]);
	if (paramCount >= 3)
	{
		job->callback = AmxStringParam(amx, params[3]);
	}
	if (paramCount >= 4)
	{
		std::string fmt = AmxStringParam(amx, params[4]);
		DecodeFormat(amx, params, 5, fmt, job->args);
	}

	// Bind to the first integer argument, if any.
	for (const CallbackArg& a : job->args)
	{
		if (a.type == CallbackArg::Type::Int)
		{
			job->boundPlayer = a.intValue;
			break;
		}
	}

	std::string sql = job->sql;
	std::string cb = job->callback;
	int handle = job->connectionHandle;
	MySQLComponent::GuardReject reason = MySQLComponent::GuardReject::None;
	if (mysqlc()->enqueueJob(std::move(job), &reason))
	{
		return 1;
	}
	ReportGuardReject(amx, handle, sql, cb, reason);
	return 0;
}

// Write a Pawn string into the AMX cell array at `dest` (max `maxlen` cells,
// including the terminator).
static void WriteAmxString(AMX* amx, cell dest, const std::string& s, int maxlen)
{
	cell* addr = nullptr;
	if (maxlen <= 0 || amx_GetAddr(amx, dest, &addr) != AMX_ERR_NONE || addr == nullptr)
	{
		return;
	}
	amx_SetString(addr, s.c_str(), 0, 0, static_cast<size_t>(maxlen));
}

// mysql_format(MySQL:handle, output[], maxlen, const format[], {Float,_}:...)
//   %s string   %d/%i int   %f float   %e escaped string   %q escaped identifier
//   %% literal percent
// AMX convention: params[0]=byte-count, params[1]=FIRST arg.
// params: [0]=count [1]=handle [2]=output [3]=maxlen [4]=format [5+]=values
static cell AMX_NATIVE_CALL n_mysql_format(AMX* amx, const cell* params)
{
	int paramCount = static_cast<int>(params[0] / static_cast<cell>(sizeof(cell)));
	if (paramCount < 4)
	{
		return 0;
	}

	int handle = static_cast<int>(params[1]);
	int maxlen = static_cast<int>(params[3]);
	std::string fmt = AmxStringParam(amx, params[4]);
	Connection* conn = mysqlc()->getConnection(handle);

	std::string out;
	int nextValue = 5; // first vararg value
	char numbuf[64];

	for (size_t i = 0; i < fmt.size(); ++i)
	{
		if (fmt[i] != '%')
		{
			out += fmt[i];
			continue;
		}
		if (i + 1 >= fmt.size())
		{
			break;
		}
		char spec = fmt[++i];
		bool needValue = (spec != '%');
		if (needValue && nextValue > paramCount)
		{
			// Not enough values supplied; emit the spec literally and stop
			// consuming so the script sees its mistake rather than reading junk.
			out += '%';
			out += spec;
			continue;
		}
		switch (spec)
		{
		case '%':
			out += '%';
			break;
		case 'd':
		case 'i':
		{
			// Variadic arg -> passed by reference; deref the AMX address.
			cell* addr = nullptr;
			int v = 0;
			if (amx_GetAddr(amx, params[nextValue++], &addr) == AMX_ERR_NONE && addr != nullptr)
			{
				v = static_cast<int>(*addr);
			}
			std::snprintf(numbuf, sizeof numbuf, "%d", v);
			out += numbuf;
			break;
		}
		case 'f':
		{
			cell* addr = nullptr;
			float f = 0.0f;
			if (amx_GetAddr(amx, params[nextValue++], &addr) == AMX_ERR_NONE && addr != nullptr)
			{
				cell c = *addr;
				std::memcpy(&f, &c, sizeof f);
			}
			std::snprintf(numbuf, sizeof numbuf, "%f", f);
			out += numbuf;
			break;
		}
		case 's':
			out += AmxStringParam(amx, params[nextValue++]);
			break;
		case 'e': // escaped string value (safe inside single quotes)
		{
			std::string v = AmxStringParam(amx, params[nextValue++]);
			out += conn ? conn->escape(v) : v;
			break;
		}
		case 'q': // escaped `identifier` (table/column) — backtick-quoted
		{
			std::string v = AmxStringParam(amx, params[nextValue++]);
			// Per MySQL, a backtick inside an identifier is doubled.
			std::string q;
			q.reserve(v.size() + 2);
			q += '`';
			for (char ch : v)
			{
				if (ch == '`')
				{
					q += '`';
				}
				q += ch;
			}
			q += '`';
			out += q;
			break;
		}
		default:
			// Unknown specifier: keep it literal.
			out += '%';
			out += spec;
			break;
		}
	}

	WriteAmxString(amx, params[2], out, maxlen);
	return static_cast<cell>(out.size());
}

// mysql_stmt_execute(PreparedStatement:stmt, callback[]="", format[]="", {Float,_}:...)
// Executes the bound statement asynchronously on its connection's worker; the
// result becomes the active cache inside the callback (same as tquery).
// params: [1]=count [2]=stmt [3]=callback [4]=format [5+]=values
static cell AMX_NATIVE_CALL n_mysql_stmt_execute(AMX* amx, const cell* params)
{
	// AMX calling convention: params[0] = arg byte-count, params[1] = FIRST arg.
	// So stmt is params[1], callback params[2], format params[3], values from [4].
	int paramCount = static_cast<int>(params[0] / static_cast<cell>(sizeof(cell)));
	if (paramCount < 1)
	{
		return 0;
	}
	int stmtHandle = static_cast<int>(params[1]);
	PreparedStatement* stmt = mysqlc()->getStatement(stmtHandle);
	if (stmt == nullptr || !stmt->valid())
	{
		mysqlc()->debugLog(StringView(std::string(
			"stmt_execute reject: stmt=") + std::to_string(stmtHandle)
			+ (stmt == nullptr ? " getStatement=null" : " valid=false")));
		return 0;
	}

	auto job = std::make_unique<QueryJob>();
	job->statement = stmt;
	job->sql = stmt->queryText(); // for the OnQueryExecute audit hook
	job->connectionHandle = 0; // resolved below from the statement's connection
	if (paramCount >= 2)
	{
		job->callback = AmxStringParam(amx, params[2]);
	}
	if (paramCount >= 3)
	{
		std::string fmt = AmxStringParam(amx, params[3]);
		DecodeFormat(amx, params, 4, fmt, job->args);
	}

	// The job must run on the worker that owns this statement's connection.
	int connHandle = mysqlc()->connectionHandleOf(stmt->connection());
	if (connHandle == 0)
	{
		mysqlc()->debugLog(StringView(std::string(
			"stmt_execute reject: connectionHandleOf==0 (statement's connection not "
			"in the pool) stmt=") + std::to_string(stmtHandle)));
		return 0;
	}
	job->connectionHandle = connHandle;
	mysqlc()->debugLog(StringView(std::string("stmt_execute: enqueue stmt=")
		+ std::to_string(stmtHandle) + " conn=" + std::to_string(connHandle)
		+ " cb='" + job->callback + "'"));

	std::string cb = job->callback;
	MySQLComponent::GuardReject reason = MySQLComponent::GuardReject::None;
	if (mysqlc()->enqueueJob(std::move(job), &reason))
	{
		return 1;
	}
	mysqlc()->debugLog(StringView(std::string("stmt_execute reject: enqueueJob failed reason=")
		+ std::to_string(static_cast<int>(reason)) + " conn=" + std::to_string(connHandle)));
	ReportGuardReject(amx, connHandle, std::string(), cb, reason);
	return 0;
}

// --- VECTOR helpers (MySQL 9.x) — pure string<->float conversion -----------

// mysql_vector_to_string(const Float:values[], count, destination[], maxlength)
// Packs floats into the "[x,y,z]" textual form STRING_TO_VECTOR() accepts.
// AMX convention: params[0]=byte-count, params[1]=FIRST arg.
// params: [0]=count [1]=values[] [2]=count [3]=destination[] [4]=maxlength
static cell AMX_NATIVE_CALL n_mysql_vector_to_string(AMX* amx, const cell* params)
{
	cell* values = nullptr;
	if (amx_GetAddr(amx, params[1], &values) != AMX_ERR_NONE || values == nullptr)
	{
		return 0;
	}
	int count = static_cast<int>(params[2]);
	int maxlen = static_cast<int>(params[4]);

	std::string out = "[";
	char numbuf[64];
	for (int i = 0; i < count; ++i)
	{
		float f = 0.0f;
		cell c = values[i];
		std::memcpy(&f, &c, sizeof f);
		std::snprintf(numbuf, sizeof numbuf, "%g", f);
		if (i > 0)
		{
			out += ',';
		}
		out += numbuf;
	}
	out += ']';

	WriteAmxString(amx, params[3], out, maxlen);
	return static_cast<cell>(out.size());
}

// Parse "[x,y,z]" (whitespace tolerant) into floats; returns the count parsed.
static int ParseVectorString(const std::string& src, std::vector<float>& out)
{
	out.clear();
	size_t i = 0;
	while (i < src.size() && src[i] != '[')
	{
		++i;
	}
	if (i < src.size())
	{
		++i; // skip '['
	}
	std::string num;
	auto flush = [&]()
	{
		// trim
		size_t a = num.find_first_not_of(" \t");
		size_t b = num.find_last_not_of(" \t");
		if (a != std::string::npos)
		{
			out.push_back(static_cast<float>(std::atof(num.substr(a, b - a + 1).c_str())));
		}
		num.clear();
	};
	for (; i < src.size(); ++i)
	{
		char ch = src[i];
		if (ch == ']')
		{
			break;
		}
		if (ch == ',')
		{
			flush();
		}
		else
		{
			num += ch;
		}
	}
	if (!num.empty())
	{
		flush();
	}
	return static_cast<int>(out.size());
}

// mysql_string_to_vector(const source[], Float:destination[], maxlength)
// AMX convention: params[0]=byte-count, params[1]=FIRST arg.
// params: [0]=count [1]=source[] [2]=destination[] [3]=maxlength
static cell AMX_NATIVE_CALL n_mysql_string_to_vector(AMX* amx, const cell* params)
{
	std::string src = AmxStringParam(amx, params[1]);
	cell* dest = nullptr;
	if (amx_GetAddr(amx, params[2], &dest) != AMX_ERR_NONE || dest == nullptr)
	{
		return 0;
	}
	int maxlen = static_cast<int>(params[3]);

	std::vector<float> vals;
	ParseVectorString(src, vals);
	int n = 0;
	for (; n < static_cast<int>(vals.size()) && n < maxlen; ++n)
	{
		float f = vals[n];
		cell c;
		std::memcpy(&c, &f, sizeof c);
		dest[n] = c;
	}
	return n;
}

// mysql_vector_dim(const source[]) -> component count
static cell AMX_NATIVE_CALL n_mysql_vector_dim(AMX* amx, const cell* params)
{
	std::string src = AmxStringParam(amx, params[1]); // params[1] = first arg
	std::vector<float> vals;
	return ParseVectorString(src, vals);
}

// --- Password hashing (async; runs on the hashing thread pool) -------------

// mysql_hash(password[], callback[], format[]="", algo=HASH_ARGON2ID, {Float,_}:...)
// AMX convention: params[0]=byte-count, params[1]=FIRST arg.
// params: [0]=count [1]=password [2]=callback [3]=format [4]=algo [5+]=values
static cell AMX_NATIVE_CALL n_mysql_hash_password(AMX* amx, const cell* params)
{
	int paramCount = static_cast<int>(params[0] / static_cast<cell>(sizeof(cell)));
	if (paramCount < 2)
	{
		return 0;
	}
	auto job = std::make_unique<HashJob>();
	job->op = HashJob::Op::Hash;
	job->password = AmxStringParam(amx, params[1]);
	job->callback = AmxStringParam(amx, params[2]);
	// params[4] is algo (currently always Argon2id; bcrypt is a later addition).
	if (paramCount >= 3)
	{
		std::string fmt = AmxStringParam(amx, params[3]);
		DecodeFormat(amx, params, 5, fmt, job->args);
	}
	mysqlc()->enqueueHash(std::move(job));
	return 1;
}

// mysql_verify(password[], hash[], callback[], format[]="", {Float,_}:...)
// AMX convention: params[0]=byte-count, params[1]=FIRST arg.
// params: [0]=count [1]=password [2]=hash [3]=callback [4]=format [5+]=values
static cell AMX_NATIVE_CALL n_mysql_verify_password(AMX* amx, const cell* params)
{
	int paramCount = static_cast<int>(params[0] / static_cast<cell>(sizeof(cell)));
	if (paramCount < 3)
	{
		return 0;
	}
	auto job = std::make_unique<HashJob>();
	job->op = HashJob::Op::Verify;
	job->password = AmxStringParam(amx, params[1]);
	job->hash = AmxStringParam(amx, params[2]);
	job->callback = AmxStringParam(amx, params[3]);
	if (paramCount >= 5)
	{
		std::string fmt = AmxStringParam(amx, params[4]);
		DecodeFormat(amx, params, 5, fmt, job->args);
	}
	mysqlc()->enqueueHash(std::move(job));
	return 1;
}

// Model operation natives are defined later in the file; forward-declare them
// for the registration table below.
static cell AMX_NATIVE_CALL n_mysql_model_find(AMX* amx, const cell* params);
static cell AMX_NATIVE_CALL n_mysql_model_insert(AMX* amx, const cell* params);
static cell AMX_NATIVE_CALL n_mysql_model_update(AMX* amx, const cell* params);
static cell AMX_NATIVE_CALL n_mysql_model_delete(AMX* amx, const cell* params);
static cell AMX_NATIVE_CALL n_mysql_model_save(AMX* amx, const cell* params);

static const AMX_NATIVE_INFO kClassicNatives[] = {
	{ "mysql_config_set", n_mysql_config_set },
	{ "mysql_execute", n_mysql_execute },
	{ "mysql_execute_for", n_mysql_execute_for },
	{ "mysql_format", n_mysql_format },
	{ "mysql_stmt_execute", n_mysql_stmt_execute },
	{ "mysql_vector_to_string", n_mysql_vector_to_string },
	{ "mysql_string_to_vector", n_mysql_string_to_vector },
	{ "mysql_vector_dim", n_mysql_vector_dim },
	{ "mysql_hash", n_mysql_hash_password },
	{ "mysql_verify", n_mysql_verify_password },
	{ "mysql_model_find", n_mysql_model_find },
	{ "mysql_model_insert", n_mysql_model_insert },
	{ "mysql_model_update", n_mysql_model_update },
	{ "mysql_model_delete", n_mysql_model_delete },
	{ "mysql_model_save", n_mysql_model_save },
	{ nullptr, nullptr },
};

void MySQL_RegisterClassicNatives(AMX* amx)
{
	amx_Register(amx, kClassicNatives, -1);
}

// MySQL:mysql_connect(host[], user[], password[], database[], MySQLConfig:options)
SCRIPT_API(mysql_connect, int(std::string const& host, std::string const& user, std::string const& password, std::string const& database, int options))
{
	ConnectionOptions opts;
	if (options != 0)
	{
		ConnectionOptions* stored = mysqlc()->getOptions(options);
		if (stored != nullptr)
		{
			opts = *stored; // inherit TLS/port/timeouts/compression
		}
	}
	// ${VAR} expansion so credentials can come from the environment instead of
	// being baked into the (shippable) gamemode source. A literal value with no
	// ${...} passes through unchanged.
	opts.host = ExpandEnv(host);
	opts.user = ExpandEnv(user);
	opts.password = ExpandEnv(password);
	opts.database = ExpandEnv(database);
	return DoConnect(std::move(opts));
}

// MySQL:mysql_connect_config(file_name[])
//
// Reads `key=value` lines (host/user/password/database/port/ssl_mode/ssl_ca/
// ssl_cert/ssl_key/ssl_crl/tls_version/compression/auto_reconnect). Values
// support ${ENV_VAR} expansion so secrets stay out of the file/source.
SCRIPT_API(mysql_connect_config, int(std::string const& file_name))
{
	std::ifstream in(file_name, std::ios::binary);
	if (!in.is_open())
	{
		std::string err = "mysql_connect_config: cannot open " + file_name;
		mysqlc()->log(LogLevel::Error, StringView(err));
		return 0;
	}
	// Read the whole file, then transparently de-obfuscate if it's an OMPMENC1
	// blob (plaintext .ini passes through unchanged). Parse from the decoded text.
	std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	std::string decoded = CfgDecodeIfNeeded(raw);
	std::istringstream cfg(decoded);

	ConnectionOptions opts;
	bool cfgDebug = false;       // opt-in via `debug = 1` in the config file
	std::string cfgDebugFile;    // optional `debug_log = <file>`
	std::string line;
	while (std::getline(cfg, line))
	{
		// Strip comments (# or ;) and surrounding whitespace.
		size_t hash = line.find_first_of("#;");
		if (hash != std::string::npos)
		{
			line = line.substr(0, hash);
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos)
		{
			continue;
		}
		auto trim = [](std::string s)
		{
			size_t a = s.find_first_not_of(" \t\r\n");
			size_t b = s.find_last_not_of(" \t\r\n");
			return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
		};
		std::string key = trim(line.substr(0, eq));
		std::string val = ExpandEnv(trim(line.substr(eq + 1)));

		if (key == "host") opts.host = val;
		else if (key == "user") opts.user = val;
		else if (key == "password") opts.password = val;
		else if (key == "database") opts.database = val;
		else if (key == "port") opts.port = static_cast<unsigned int>(std::atoi(val.c_str()));
		else if (key == "ssl_mode")
		{
			int m = std::atoi(val.c_str());
			if (m >= 0 && m <= 2) opts.sslMode = static_cast<SSLMode>(m);
		}
		else if (key == "ssl_ca") opts.sslCA = val;
		else if (key == "ssl_cert") opts.sslCert = val;
		else if (key == "ssl_key") opts.sslKey = val;
		else if (key == "ssl_crl") opts.sslCRL = val;
		else if (key == "tls_version") opts.tlsVersion = val;
		else if (key == "compression") opts.compression = val;
		else if (key == "auto_reconnect") opts.autoReconnect = (val == "1" || val == "true");
		// Operator-controlled debug logging straight from the config file, so it's
		// opt-in (default OFF) without touching the gamemode. `debug = 1|true`
		// turns on "[omp-MySQL]" logging; optional `debug_log = <file>` also mirrors
		// it to a server-side file.
		else if (key == "debug") cfgDebug = (val == "1" || val == "true");
		else if (key == "debug_log") cfgDebugFile = val;
	}

	// Apply config-driven debug before connecting, so the connect itself is logged.
	if (cfgDebug)
	{
		mysqlc()->setDebug(true, StringView(cfgDebugFile));
	}

	return DoConnect(std::move(opts));
}

// mysql_config_obfuscate(const plain_ini[], const out_file[])
// Read a plaintext .ini and write an obfuscated OMPMENC1 blob that
// mysql_connect_config() reads transparently. A one-time authoring helper so the
// shipped config isn't plaintext. HONEST: this is obfuscation, not encryption —
// the key is in the binary. Returns true on success. Run once (e.g. from a tools
// script), then delete the plaintext and ship only the .enc.
SCRIPT_API(mysql_config_obfuscate, bool(std::string const& plain_ini, std::string const& out_file))
{
	std::ifstream in(plain_ini, std::ios::binary);
	if (!in.is_open())
	{
		mysqlc()->log(LogLevel::Error,
			StringView("mysql_config_obfuscate: cannot open " + plain_ini));
		return false;
	}
	std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	// Don't double-encode an already-obfuscated file.
	const size_t magicLen = sizeof(kCfgMagic) - 1;
	if (raw.size() >= magicLen && raw.compare(0, magicLen, kCfgMagic) == 0)
	{
		mysqlc()->log(LogLevel::Warning, "mysql_config_obfuscate: input already obfuscated");
		return false;
	}
	std::ofstream out(out_file, std::ios::binary | std::ios::trunc);
	if (!out.is_open())
	{
		mysqlc()->log(LogLevel::Error,
			StringView("mysql_config_obfuscate: cannot write " + out_file));
		return false;
	}
	bool fromEnv = false;
	CfgKeySeed(&fromEnv);
	if (!fromEnv)
	{
		mysqlc()->log(LogLevel::Warning,
			"mysql_config_obfuscate: OMP_CFG_KEY not set — using the WEAK built-in "
			"key. Set OMP_CFG_KEY (a per-server secret) in the environment so the "
			"blob isn't trivially reversible from the public source.");
	}
	out.write(kCfgMagic, static_cast<std::streamsize>(magicLen));
	std::string enc = CfgXor(raw);
	out.write(enc.data(), static_cast<std::streamsize>(enc.size()));
	return static_cast<bool>(out);
}

// mysql_close(MySQL:handle)
SCRIPT_API(mysql_close, bool(int handle))
{
	return mysqlc()->removeConnection(handle);
}

// mysql_errno(MySQL:handle)
SCRIPT_API(mysql_errno, int(int handle))
{
	Connection* c = mysqlc()->getConnection(handle);
	return c ? static_cast<int>(c->lastErrno()) : 0;
}

// mysql_error(destination[], maxlength, MySQL:handle)
SCRIPT_API(mysql_error, bool(OutputOnlyString& destination, int handle))
{
	Connection* c = mysqlc()->getConnection(handle);
	std::string err = c ? c->lastError() : std::string("invalid connection handle");
	destination = err;
	return true;
}

// ===========================================================================
//  Version / update check
// ===========================================================================

// Version constants live in MySQLComponent (kVersion / kLatestVersion) so the
// load banner, the server-log update notice, and these natives share one source.

// mysql_version(destination[], maxlength) — this build's version (e.g. "0.1.0").
SCRIPT_API(mysql_version, bool(OutputOnlyString& destination, int maxlength))
{
	(void)maxlength;
	destination = std::string(MySQLComponent::kVersion);
	return true;
}

// mysql_latest_version(destination[], maxlength) — the latest published version
// this build is aware of. Compare with mysql_version() to detect updates.
SCRIPT_API(mysql_latest_version, bool(OutputOnlyString& destination, int maxlength))
{
	(void)maxlength;
	destination = std::string(MySQLComponent::kLatestVersion);
	return true;
}

// mysql_update_available() — true if LATEST is newer than this build's version.
SCRIPT_API(mysql_update_available, bool())
{
	return MySQLComponent::updateAvailable();
}

// ===========================================================================
//  Debug logging
// ===========================================================================

// mysql_debug(bool:enable, const logfile[] = "")
// Toggle verbose "[omp-MySQL]" diagnostics (connections, queries, errors, the
// prepared-statement path) to the console. If `logfile` is non-empty, debug lines
// are also appended (timestamped) to that server-side file. Returns true.
SCRIPT_API(mysql_debug, bool(bool enable, std::string const& logfile))
{
	mysqlc()->setDebug(enable, StringView(logfile));
	if (!enable)
	{
		// Always announce the state change once, even when turning OFF.
		mysqlc()->log(LogLevel::Message, StringView("debug logging disabled"));
	}
	return true;
}

// mysql_debug_enabled() — true if debug logging is currently on.
SCRIPT_API(mysql_debug_enabled, bool())
{
	return mysqlc()->debugEnabled();
}

// mysql_log(const message[]) — emit a script-supplied "[omp-MySQL]" debug line
// (no-op unless debug is enabled). Lets a gamemode funnel its own diagnostics
// through the same tagged console/file sink.
SCRIPT_API(mysql_log, bool(std::string const& message))
{
	mysqlc()->debugLog(StringView(message));
	return true;
}

// ===========================================================================
//  TLS introspection
// ===========================================================================

// mysql_get_tls_cipher(destination[], maxlength, MySQL:handle)
SCRIPT_API(mysql_get_tls_cipher, bool(OutputOnlyString& destination, int handle))
{
	Connection* c = mysqlc()->getConnection(handle);
	destination = c ? c->sslCipher() : std::string();
	return true;
}

// mysql_is_tls_enabled(MySQL:handle)
SCRIPT_API(mysql_is_tls_enabled, bool(int handle))
{
	Connection* c = mysqlc()->getConnection(handle);
	return c && c->sslEnabled();
}

// ===========================================================================
//  Queries
//
//  mysql_execute / mysql_execute_for are classic AMX natives (variadic + format
//  mini-language) defined above and registered in kClassicNatives.
// ===========================================================================

// mysql_execute_sync(MySQL:handle, query[], bool:use_cache) -> ResultSet:
// Synchronous; runs the query but does not yet expose results (returns 1/0).
// The ResultSet: return type is honoured once the cache API lands in Phase 4.
SCRIPT_API(mysql_execute_sync, int(int handle, std::string const& query, bool use_cache))
{
	(void)use_cache;
	Connection* c = mysqlc()->getConnection(handle);
	if (!c)
	{
		return 0;
	}
	// Same multi-statement-off defense the async path enforces in enqueueJob:
	// reject stacked statements on a connection that didn't opt in. The sync
	// path can't reach enqueueJob, so guard it here directly.
	if (!mysqlc()->multiStatementsAllowed(handle)
		&& MySQLComponent::isMultiStatement(query))
	{
		mysqlc()->log(LogLevel::Warning,
			"mysql_execute_sync rejected: multiple statements on a connection that did "
			"not enable CLIENT_MULTI_STATEMENTS (possible SQL injection)");
		return 0;
	}
	auto start = std::chrono::steady_clock::now();
	bool ok = c->query(query);
	auto end = std::chrono::steady_clock::now();
	if (ok)
	{
		// Audit hook, same contract as the async path (real measured time).
		auto micros = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
		mysqlc()->fireQueryExecuteHook(handle, query, static_cast<long long>(micros));
	}
	return ok ? 1 : 0;
}

// mysql_pending_count(MySQL:handle) -> queued/in-flight async query count
SCRIPT_API(mysql_pending_count, int(int handle))
{
	return mysqlc()->pendingQueries(handle);
}

// mysql_set_limit(MySQL:handle, E_MYSQL_LIMIT:type, value) -> bool
// Configure a query guard at runtime. type mirrors E_MYSQL_LIMIT in omp-mysql.inc:
//   0 LIMIT_MAX_LENGTH    value = max query bytes (0 = no limit)
//   1 LIMIT_MAX_PENDING   value = max queued/in-flight jobs (0 = no limit)
//   2 LIMIT_RATE_PER_SEC  value = max queries per second (0 = no rate limit)
SCRIPT_API(mysql_set_limit, bool(int handle, int type, int value))
{
	QueryGuards* g = mysqlc()->guardsFor(handle);
	if (g == nullptr || value < 0)
	{
		return false;
	}
	switch (type)
	{
	case 0: g->maxQueryLength = static_cast<std::size_t>(value); return true;
	case 1: g->maxPendingQueries = value; return true;
	case 2:
		g->maxQueriesPerWindow = static_cast<unsigned int>(value);
		g->rateWindow = std::chrono::milliseconds(1000);
		return true;
	default:
		return false;
	}
}

// ===========================================================================
//  Escaping, charset, status
//
//  Prefer prepared statements over manual escaping; these are for when you
//  must build a query string yourself.  mysql_format (above) is the variadic
//  builder with %e/%q escaping.
// ===========================================================================

// mysql_escape(source[], destination[], maxlength, MySQL:handle)
SCRIPT_API(mysql_escape, int(std::string const& source, OutputOnlyString& destination, int maxlength, int handle))
{
	(void)maxlength;
	Connection* c = mysqlc()->getConnection(handle);
	std::string escaped = c ? c->escape(source) : source;
	destination = escaped;
	return static_cast<int>(escaped.size());
}

// mysql_set_charset(charset[], MySQL:handle)
SCRIPT_API(mysql_set_charset, bool(std::string const& charset, int handle))
{
	Connection* c = mysqlc()->getConnection(handle);
	return c && c->setCharset(charset);
}

// mysql_get_charset(destination[], maxlength, MySQL:handle)
SCRIPT_API(mysql_get_charset, bool(OutputOnlyString& destination, int maxlength, int handle))
{
	(void)maxlength;
	Connection* c = mysqlc()->getConnection(handle);
	destination = c ? c->getCharset() : std::string();
	return c != nullptr;
}

// mysql_server_status(destination[], maxlength, MySQL:handle)
SCRIPT_API(mysql_server_status, bool(OutputOnlyString& destination, int maxlength, int handle))
{
	(void)maxlength;
	Connection* c = mysqlc()->getConnection(handle);
	destination = c ? c->stat() : std::string();
	return c != nullptr;
}

// ===========================================================================
//  Result cache — reading the active result set (cache_* natives)
//
//  The "active" result set is the one the running query callback owns; the
//  component sets it before the callback and clears it after. To keep a result
//  beyond the callback, mysql_rs_retain() it and mysql_rs_activate() it later.
// ===========================================================================

// Helpers operating on the active result set.
static const ResultSet* activeSet()
{
	return mysqlc()->activeSet();
}

// mysql_rs_row_count(&destination)
SCRIPT_API(mysql_rs_row_count, bool(cell& destination))
{
	const ResultSet* s = activeSet();
	destination = s ? s->rowCount() : 0;
	return s != nullptr;
}

// mysql_rs_field_count(&destination)
SCRIPT_API(mysql_rs_field_count, bool(cell& destination))
{
	const ResultSet* s = activeSet();
	destination = s ? s->fieldCount() : 0;
	return s != nullptr;
}

// mysql_rs_set_count(&destination)
SCRIPT_API(mysql_rs_set_count, bool(cell& destination))
{
	const QueryResult* r = mysqlc()->activeResult();
	destination = r ? r->resultCount() : 0;
	return r != nullptr;
}

// mysql_rs_field_name(field_index, destination[], maxlength)
SCRIPT_API(mysql_rs_field_name, bool(int field_index, OutputOnlyString& destination))
{
	const ResultSet* s = activeSet();
	if (!s || field_index < 0 || field_index >= s->fieldCount())
	{
		destination = std::string();
		return false;
	}
	destination = s->fields[field_index].name;
	return true;
}

// E_MYSQL_FIELD_TYPE:mysql_rs_field_type(field_index)
SCRIPT_API(mysql_rs_field_type, int(int field_index))
{
	const ResultSet* s = activeSet();
	if (!s || field_index < 0 || field_index >= s->fieldCount())
	{
		return -1; // MYSQL_TYPE_INVALID
	}
	return s->fields[field_index].type;
}

// mysql_rs_select_set(result_index)
SCRIPT_API(mysql_rs_select_set, bool(int result_index))
{
	return mysqlc()->setActiveResultIndex(result_index);
}

// --- value getters: by column index ---

// Resolve (row, column) -> ResultValue*, or nullptr if out of range.
static const ResultValue* cellAt(int row, int column)
{
	const ResultSet* s = activeSet();
	if (!s || row < 0 || row >= s->rowCount() || column < 0 || column >= s->fieldCount())
	{
		return nullptr;
	}
	return &s->rows[row][column];
}

// mysql_rs_get_string(row, column, destination[], maxlength)
SCRIPT_API(mysql_rs_get_string, bool(int row, int column, OutputOnlyString& destination))
{
	const ResultValue* v = cellAt(row, column);
	destination = (v && !v->isNull) ? v->value : std::string();
	return v != nullptr;
}

// mysql_rs_get_int(row, column, &destination)
SCRIPT_API(mysql_rs_get_int, bool(int row, int column, cell& destination))
{
	const ResultValue* v = cellAt(row, column);
	destination = (v && !v->isNull) ? static_cast<cell>(std::atoi(v->value.c_str())) : 0;
	return v != nullptr;
}

// mysql_rs_get_float(row, column, &Float:destination)
SCRIPT_API(mysql_rs_get_float, bool(int row, int column, float& destination))
{
	const ResultValue* v = cellAt(row, column);
	destination = (v && !v->isNull) ? static_cast<float>(std::atof(v->value.c_str())) : 0.0f;
	return v != nullptr;
}

// mysql_rs_get_bool(row, column, &bool:destination)
SCRIPT_API(mysql_rs_get_bool, bool(int row, int column, bool& destination))
{
	const ResultValue* v = cellAt(row, column);
	destination = (v && !v->isNull) ? (std::atoi(v->value.c_str()) != 0) : false;
	return v != nullptr;
}

// mysql_rs_is_null(row, column, &bool:destination)
SCRIPT_API(mysql_rs_is_null, bool(int row, int column, bool& destination))
{
	const ResultValue* v = cellAt(row, column);
	destination = v ? v->isNull : true;
	return v != nullptr;
}

// --- value getters: by column name ---

static const ResultValue* cellByName(int row, const std::string& column)
{
	const ResultSet* s = activeSet();
	if (!s)
	{
		return nullptr;
	}
	int col = s->fieldIndex(column);
	if (col < 0)
	{
		return nullptr;
	}
	return cellAt(row, col);
}

// mysql_rs_get_string_by(row, column[], destination[], maxlength)
SCRIPT_API(mysql_rs_get_string_by, bool(int row, std::string const& column, OutputOnlyString& destination))
{
	const ResultValue* v = cellByName(row, column);
	destination = (v && !v->isNull) ? v->value : std::string();
	return v != nullptr;
}

// mysql_rs_get_int_by(row, column[], &destination)
SCRIPT_API(mysql_rs_get_int_by, bool(int row, std::string const& column, cell& destination))
{
	const ResultValue* v = cellByName(row, column);
	destination = (v && !v->isNull) ? static_cast<cell>(std::atoi(v->value.c_str())) : 0;
	return v != nullptr;
}

// mysql_rs_get_float_by(row, column[], &Float:destination)
SCRIPT_API(mysql_rs_get_float_by, bool(int row, std::string const& column, float& destination))
{
	const ResultValue* v = cellByName(row, column);
	destination = (v && !v->isNull) ? static_cast<float>(std::atof(v->value.c_str())) : 0.0f;
	return v != nullptr;
}

// mysql_rs_get_bool_by(row, column[], &bool:destination)
SCRIPT_API(mysql_rs_get_bool_by, bool(int row, std::string const& column, bool& destination))
{
	const ResultValue* v = cellByName(row, column);
	destination = (v && !v->isNull) ? (std::atoi(v->value.c_str()) != 0) : false;
	return v != nullptr;
}

// mysql_rs_is_null_by(row, column[], &bool:destination)
SCRIPT_API(mysql_rs_is_null_by, bool(int row, std::string const& column, bool& destination))
{
	const ResultValue* v = cellByName(row, column);
	destination = v ? v->isNull : true;
	return v != nullptr;
}

// --- saving / activating result sets ---

// ResultSet:mysql_rs_retain()
SCRIPT_API(mysql_rs_retain, int())
{
	return mysqlc()->saveActiveCache();
}

// mysql_rs_release(ResultSet:rs_id)
SCRIPT_API(mysql_rs_release, bool(int rs_id))
{
	return mysqlc()->deleteSavedCache(rs_id);
}

// mysql_rs_activate(ResultSet:rs_id)
SCRIPT_API(mysql_rs_activate, bool(int rs_id))
{
	return mysqlc()->activateSavedCache(rs_id);
}

// mysql_rs_deactivate()
SCRIPT_API(mysql_rs_deactivate, bool())
{
	mysqlc()->clearActiveCache();
	return true;
}

// mysql_rs_is_active()
SCRIPT_API(mysql_rs_is_active, bool())
{
	return mysqlc()->hasActiveCache();
}

// mysql_rs_is_valid(ResultSet:rs_id)
SCRIPT_API(mysql_rs_is_valid, bool(int rs_id))
{
	return mysqlc()->isValidCache(rs_id);
}

// --- per-query metadata ---

// mysql_rs_affected_rows()
SCRIPT_API(mysql_rs_affected_rows, int())
{
	const ResultSet* s = activeSet();
	return s ? static_cast<int>(s->affectedRows) : 0;
}

// mysql_rs_insert_id()
SCRIPT_API(mysql_rs_insert_id, int())
{
	const ResultSet* s = activeSet();
	return s ? static_cast<int>(s->insertId) : 0;
}

// mysql_rs_warning_count()
SCRIPT_API(mysql_rs_warning_count, int())
{
	const ResultSet* s = activeSet();
	return s ? static_cast<int>(s->warningCount) : 0;
}

// mysql_rs_exec_time(E_MYSQL_EXECTIME_UNIT:unit) — 0=micros, 1=millis
SCRIPT_API(mysql_rs_exec_time, int(int unit))
{
	const QueryResult* r = mysqlc()->activeResult();
	if (!r)
	{
		return 0;
	}
	int64_t us = r->execTimeMicros;
	return static_cast<int>(unit == 1 ? (us / 1000) : us);
}

// mysql_rs_query(destination[], maxlength)
SCRIPT_API(mysql_rs_query, bool(OutputOnlyString& destination))
{
	const QueryResult* r = mysqlc()->activeResult();
	destination = r ? r->queryString : std::string();
	return r != nullptr;
}

// MySQL_Count(what) — internal backing for the mysql_rs_num_* macros
// (0 = rows, 1 = fields, 2 = results).
SCRIPT_API(MySQL_Count, int(int what))
{
	if (what == 2)
	{
		const QueryResult* r = mysqlc()->activeResult();
		return r ? r->resultCount() : 0;
	}
	const ResultSet* s = activeSet();
	if (!s)
	{
		return 0;
	}
	return what == 1 ? s->fieldCount() : s->rowCount();
}

// ===========================================================================
//  Prepared statements (injection-safe)
//
//  Prepare once, bind typed params to the ? placeholders, then execute. The
//  execute native (mysql_stmt_execute) is classic/variadic and declared above.
// ===========================================================================

// PreparedStatement:mysql_prepare(MySQL:handle, query[])
SCRIPT_API(mysql_prepare, int(int handle, std::string const& query))
{
	Connection* c = mysqlc()->getConnection(handle);
	if (c == nullptr)
	{
		return 0; // MYSQL_INVALID_STMT
	}
	auto stmt = std::make_unique<PreparedStatement>(c, query);
	if (!stmt->valid())
	{
		std::string err = "mysql_prepare failed: " + stmt->lastError();
		mysqlc()->log(LogLevel::Error, StringView(err));
		return 0;
	}
	int h = mysqlc()->addStatement(std::move(stmt));
	mysqlc()->debugLog(StringView(std::string("mysql_prepare: handle=") + std::to_string(h)
		+ " query='" + query.substr(0, 60) + "'"));
	return h;
}

// mysql_stmt_set_int(PreparedStatement:stmt, param_index, value)
SCRIPT_API(mysql_stmt_set_int, bool(int stmt, int param_index, int value))
{
	PreparedStatement* s = mysqlc()->getStatement(stmt);
	if (s == nullptr)
	{
		return false;
	}
	s->bindInt(param_index - 1, value); // 1-based (JDBC) -> 0-based
	return true;
}

// mysql_stmt_set_float(PreparedStatement:stmt, param_index, Float:value)
SCRIPT_API(mysql_stmt_set_float, bool(int stmt, int param_index, float value))
{
	PreparedStatement* s = mysqlc()->getStatement(stmt);
	if (s == nullptr)
	{
		return false;
	}
	s->bindFloat(param_index - 1, value); // 1-based (JDBC) -> 0-based
	return true;
}

// mysql_stmt_set_string(PreparedStatement:stmt, param_index, value[])
SCRIPT_API(mysql_stmt_set_string, bool(int stmt, int param_index, std::string const& value))
{
	mysqlc()->debugLog(StringView(std::string("stmt_set_string: stmt=") + std::to_string(stmt)
		+ " idx=" + std::to_string(param_index)));
	PreparedStatement* s = mysqlc()->getStatement(stmt);
	if (s == nullptr)
	{
		return false;
	}
	s->bindString(param_index - 1, value); // 1-based (JDBC) -> 0-based
	return true;
}

// mysql_stmt_set_null(PreparedStatement:stmt, param_index)
SCRIPT_API(mysql_stmt_set_null, bool(int stmt, int param_index))
{
	PreparedStatement* s = mysqlc()->getStatement(stmt);
	if (s == nullptr)
	{
		return false;
	}
	s->bindNull(param_index - 1); // 1-based (JDBC) -> 0-based
	return true;
}

// mysql_stmt_close(PreparedStatement:stmt)
SCRIPT_API(mysql_stmt_close, bool(int stmt))
{
	return mysqlc()->removeStatement(stmt);
}

// ===========================================================================
//  Model — active-record mapping (mysql_model_*)
// ===========================================================================

// Read a bound field's current Pawn value and render it as a SQL literal:
// numbers bare, strings escaped + single-quoted (via the connection's charset).
static std::string ModelFieldLiteral(const ModelField& f, Connection* conn)
{
	if (f.addr == nullptr)
	{
		return "NULL";
	}
	switch (f.type)
	{
	case ModelField::Type::Int:
		return std::to_string(static_cast<int>(*f.addr));
	case ModelField::Type::Float:
	{
		float fv = 0.0f;
		std::memcpy(&fv, f.addr, sizeof(fv));
		return std::to_string(fv);
	}
	case ModelField::Type::String:
	default:
	{
		// Pull the AMX string out of the bound cell array.
		std::string s;
		int len = 0;
		amx_StrLen(f.addr, &len);
		s.resize(static_cast<size_t>(len), '\0');
		if (len > 0)
		{
			amx_GetString(&s[0], f.addr, 0, len + 1);
		}
		std::string esc = conn ? conn->escape(s) : s;
		return "'" + esc + "'";
	}
	}
}

// mysql_model_create(table[], MySQL:handle) -> Model:
SCRIPT_API(mysql_model_create, int(std::string const& table, int handle))
{
	if (table.empty() || mysqlc()->getConnection(handle) == nullptr)
	{
		return 0;
	}
	return mysqlc()->addModel(std::make_unique<Model>(handle, table));
}

SCRIPT_API(mysql_model_destroy, bool(int model))
{
	return mysqlc()->removeModel(model);
}

SCRIPT_API(mysql_model_errno, int(int model))
{
	Model* m = mysqlc()->getModel(model);
	return m ? static_cast<int>(m->lastError()) : static_cast<int>(ModelError::Invalid);
}

// Binds capture the bound variable's physical AMX address (&variable). The
// variable must be a global that outlives the async call.
SCRIPT_API(mysql_model_bind_int, bool(int model, cell& variable, std::string const& column))
{
	Model* m = mysqlc()->getModel(model);
	if (m == nullptr || column.empty()) return false;
	m->bindInt(column, reinterpret_cast<ModelCell*>(&variable));
	return true;
}

SCRIPT_API(mysql_model_bind_float, bool(int model, float& variable, std::string const& column))
{
	Model* m = mysqlc()->getModel(model);
	if (m == nullptr || column.empty()) return false;
	m->bindFloat(column, reinterpret_cast<ModelCell*>(&variable));
	return true;
}

SCRIPT_API(mysql_model_bind_string, bool(int model, cell& variable, int variable_maxlen, std::string const& column))
{
	Model* m = mysqlc()->getModel(model);
	if (m == nullptr || column.empty() || variable_maxlen <= 0) return false;
	m->bindString(column, reinterpret_cast<ModelCell*>(&variable), variable_maxlen);
	return true;
}

SCRIPT_API(mysql_model_unbind, bool(int model, std::string const& column))
{
	Model* m = mysqlc()->getModel(model);
	return m ? m->unbind(column) : false;
}

SCRIPT_API(mysql_model_clear, bool(int model))
{
	Model* m = mysqlc()->getModel(model);
	if (m == nullptr) return false;
	m->clear();
	return true;
}

SCRIPT_API(mysql_model_set_key, bool(int model, std::string const& column))
{
	Model* m = mysqlc()->getModel(model);
	if (m == nullptr || column.empty()) return false;
	m->setKey(column);
	return true;
}

// Shared dispatch for a model operation: build SQL from the bound vars, enqueue
// an async job tagged with the model handle + op (the result is written back in
// dispatchJob). Classic native (variadic callback args via the format lang).
static cell ModelExecute(AMX* amx, const cell* params, ModelOp op)
{
	// AMX convention: params[0]=byte-count, params[1]=FIRST arg (model handle),
	// params[2]=callback, params[3]=format, params[4+]=values.
	int paramCount = static_cast<int>(params[0] / static_cast<cell>(sizeof(cell)));
	if (paramCount < 1) return 0;
	int modelHandle = static_cast<int>(params[1]);
	Model* m = mysqlc()->getModel(modelHandle);
	if (m == nullptr) return 0;

	Connection* conn = mysqlc()->getConnection(m->connectionHandle());
	if (conn == nullptr) { m->setError(ModelError::Invalid); return 0; }

	// Render all bound values + the key value as SQL literals.
	std::vector<std::string> vals;
	vals.reserve(m->fields().size());
	for (const ModelField& f : m->fields())
	{
		vals.push_back(ModelFieldLiteral(f, conn));
	}
	std::string keyLit = "NULL";
	if (const ModelField* kf = m->keyField())
	{
		keyLit = ModelFieldLiteral(*kf, conn);
	}

	std::string sql;
	switch (op)
	{
	case MODEL_OP_FIND:   sql = m->buildFind(keyLit); break;
	case MODEL_OP_INSERT: sql = m->buildInsert(vals); break;
	case MODEL_OP_UPDATE: sql = m->buildUpdate(vals, keyLit); break;
	case MODEL_OP_DELETE: sql = m->buildDelete(keyLit); break;
	}
	if (sql.empty()) { m->setError(ModelError::Invalid); return 0; }

	auto job = std::make_unique<QueryJob>();
	job->connectionHandle = m->connectionHandle();
	job->sql = sql;
	job->modelHandle = modelHandle;
	job->modelOp = static_cast<int>(op);
	if (paramCount >= 2) job->callback = AmxStringParam(amx, params[2]);
	if (paramCount >= 3)
	{
		std::string fmt = AmxStringParam(amx, params[3]);
		DecodeFormat(amx, params, 4, fmt, job->args);
	}
	return mysqlc()->enqueueJob(std::move(job)) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_mysql_model_find(AMX* amx, const cell* params)
{
	return ModelExecute(amx, params, MODEL_OP_FIND);
}
static cell AMX_NATIVE_CALL n_mysql_model_insert(AMX* amx, const cell* params)
{
	return ModelExecute(amx, params, MODEL_OP_INSERT);
}
static cell AMX_NATIVE_CALL n_mysql_model_update(AMX* amx, const cell* params)
{
	return ModelExecute(amx, params, MODEL_OP_UPDATE);
}
static cell AMX_NATIVE_CALL n_mysql_model_delete(AMX* amx, const cell* params)
{
	return ModelExecute(amx, params, MODEL_OP_DELETE);
}
// save: insert if the bound key is unset (zero/empty), else update.
static cell AMX_NATIVE_CALL n_mysql_model_save(AMX* amx, const cell* params)
{
	int paramCount = static_cast<int>(params[0] / static_cast<cell>(sizeof(cell)));
	if (paramCount < 1) return 0;
	Model* m = mysqlc()->getModel(static_cast<int>(params[1])); // params[1]=first arg
	if (m == nullptr) return 0;
	const ModelField* kf = m->keyField();
	bool keySet = false;
	if (kf != nullptr && kf->addr != nullptr)
	{
		if (kf->type == ModelField::Type::String)
		{
			int len = 0; amx_StrLen(kf->addr, &len); keySet = len > 0;
		}
		else
		{
			keySet = (*kf->addr != 0);
		}
	}
	return ModelExecute(amx, params, keySet ? MODEL_OP_UPDATE : MODEL_OP_INSERT);
}

// mysql_hash_sync(password[], destination[], maxlength, algo)
SCRIPT_API(mysql_hash_sync, int(std::string const& password, OutputOnlyString& destination, int maxlength, int algo))
{
	(void)maxlength;
	(void)algo; // Argon2id only for now
	std::string h = Hashing::hashArgon2id(password);
	destination = h;
	return static_cast<int>(h.size());
}

// mysql_verify_sync(password[], hash[])
SCRIPT_API(mysql_verify_sync, bool(std::string const& password, std::string const& hash))
{
	return Hashing::verify(password, hash);
}
