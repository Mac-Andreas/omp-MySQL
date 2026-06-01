/* =============================================================================
 *
 *  MySQL for open.mp  —  Pawn include (API surface)
 *
 *  A modern, clean-room MySQL database component for open.mp, built against the
 *  modern MySQL (8.x/9.x) C API. This file is the public contract: every native,
 *  callback, constant and macro a gamemode uses.
 *
 *  Author : Xyranaut  (Mac Andreas)
 *  License: MIT  (see LICENSE)
 *  Notice : Independent implementation, inspired by the SA-MP-MySQL Pawn API.
 *           No upstream source was copied. See NOTICE.md.
 *
 *  Security at a glance:
 *    - Connections are ALWAYS TLS-encrypted (no plaintext path).
 *    - Prepared statements (mysql_stmt_*) are the recommended, injection-safe
 *      way to run queries — prefer them over building query strings.
 *    - Built-in Argon2id/bcrypt password hashing for login systems.
 *
 * ========================================================================== */

#if defined _INC_mysql
	#endinput
#endif
#define _INC_mysql

// Legacy include guard (old gamemodes did `#if defined mysql_included`).
#if !defined mysql_included
	#define mysql_included
#endif


/* -----------------------------------------------------------------------------
 *  Tags  —  opaque handles. Treat these as black boxes; never do maths on them.
 * -------------------------------------------------------------------------- */

/// A connection handle returned by mysql_connect().
#define MYSQL_DEFAULT_HANDLE   (MySQL:1)
#define MYSQL_INVALID_HANDLE   (MySQL:0)

/// A stored (saved) result set handle returned by mysql_rs_retain().
#define MYSQL_INVALID_CACHE    (ResultSet:0)

/// A model handle returned by mysql_model_create().
#define MYSQL_INVALID_MODEL    (Model:0)

/// A connection-options set returned by mysql_config_create().
#define MYSQL_INVALID_OPTIONS  (MySQLConfig:0)

/// A prepared-statement handle returned by mysql_prepare().
#define MYSQL_INVALID_STMT     (PreparedStatement:0)


/* -----------------------------------------------------------------------------
 *  Common MySQL error codes (subset you actually branch on in scripts).
 *  Full list: https://dev.mysql.com/doc/mysql-errors/9.0/en/
 * -------------------------------------------------------------------------- */

#define ER_DBACCESS_DENIED_ERROR (1044) // no access to the database
#define ER_ACCESS_DENIED_ERROR   (1045) // wrong user/password
#define ER_UNKNOWN_TABLE         (1109) // table does not exist
#define ER_SYNTAX_ERROR          (1149) // SQL syntax error
#define ER_DUP_ENTRY             (1062) // duplicate value for a UNIQUE/PRIMARY key
#define CR_SERVER_GONE_ERROR     (2006) // server closed the connection
#define CR_SERVER_LOST           (2013) // lost connection mid-query
#define CR_COMMAND_OUT_OF_SYNC   (2014) // commands run in the wrong order
#define CR_SSL_CONNECTION_ERROR  (2026) // TLS handshake failed (cert/CA/cipher)


/* -----------------------------------------------------------------------------
 *  Enums
 * -------------------------------------------------------------------------- */

/// Result of the last model operation (mysql_model_errno()).
enum E_MYSQL_MODEL_ERROR
{
	MODEL_OK = 0,      // success
	MODEL_NO_DATA,     // find() matched no row
	MODEL_INVALID,     // invalid model handle / column / no key set
}

/// Process-wide options set via mysql_global_set() before connecting.
enum E_MYSQL_GLOBAL_OPTION
{
	DUPLICATE_CONNECTIONS,        // bool: allow two handles to the same host/db
	DUPLICATE_CONNECTION_WARNING, // bool: warn (don't block) on duplicates
}

/// Per-connection options set via mysql_config_set() on a MySQLOpt set.
///
/// SSL/TLS note: there is intentionally NO "enable SSL" toggle. TLS is always
/// on. You only choose how strictly the certificate is verified (SSL_MODE) and
/// supply the cert/key/CA paths. See the SSL_MODE_* constants.
enum E_MYSQL_OPTION
{
	AUTO_RECONNECT,         // bool: transparently reconnect with backoff
	MULTI_STATEMENTS,       // bool: allow ';'-separated stacked queries
	                        //       (OFF by default — injection hardening)
	POOL_SIZE,              // int : worker connections for async queries
	SERVER_PORT,            // int : TCP port (default 3306)
	CONNECT_TIMEOUT,        // int : connect timeout, seconds (0 = library default)
	READ_TIMEOUT,           // int : per-read timeout, seconds
	WRITE_TIMEOUT,          // int : per-write timeout, seconds

	// --- TLS / certificates ---
	SSL_MODE,               // verification strictness (SSL_MODE_* constants)
	SSL_CA,                 // string: CA certificate file (PEM)
	SSL_CAPATH,             // string: directory of trusted CA certs
	SSL_CERT,               // string: client certificate (mutual TLS)
	SSL_KEY,                // string: client private key (mutual TLS)
	SSL_CRL,                // string: certificate revocation list file
	SSL_CRLPATH,            // string: directory of CRLs
	SSL_CIPHER,             // string: allowed TLS 1.2 cipher list
	TLS_VERSION,            // string: e.g. "TLSv1.3,TLSv1.2" (1.0/1.1 rejected)

	// --- Compression ---
	COMPRESSION,            // string: "zstd", "zlib", or "zstd,zlib"
	ZSTD_COMPRESSION_LEVEL, // int   : 1..22 (default 3)

	// --- Query guards (opt-in; 0 = unlimited) ---
	MAX_QUERIES_PER_SECOND, // int : per-connection rate cap
	MAX_QUERY_LENGTH,       // int : reject queries longer than this
	MAX_PENDING_QUERIES,    // int : cap on queued async queries
}

/// TLS certificate-verification strictness. All values are encrypted; the only
/// difference is how much of the server's certificate chain is checked.
/// (Insecure DISABLED/PREFERRED modes from raw MySQL are deliberately omitted.)
///
/// Plain (untagged) constants, not an enum, so they pass into the
/// mysql_config_set(opt, SSL_MODE, ...) value vararg with no tag-cast warning.
#define SSL_MODE_REQUIRED        (0) // encrypt; don't verify the certificate
#define SSL_MODE_VERIFY_CA       (1) // encrypt + verify cert chains to a trusted CA
#define SSL_MODE_VERIFY_IDENTITY (2) // encrypt + verify CA + hostname (full MITM safety)

/// Column data types reported by mysql_rs_field_type(). Values match the
/// MySQL C API enum_field_types so they're stable across versions.
enum E_MYSQL_FIELD_TYPE
{
	MYSQL_TYPE_INVALID    = -1,
	MYSQL_TYPE_DECIMAL    = 0,
	MYSQL_TYPE_TINY       = 1,
	MYSQL_TYPE_SHORT      = 2,
	MYSQL_TYPE_LONG       = 3,
	MYSQL_TYPE_FLOAT      = 4,
	MYSQL_TYPE_DOUBLE     = 5,
	MYSQL_TYPE_NULL       = 6,
	MYSQL_TYPE_TIMESTAMP  = 7,
	MYSQL_TYPE_LONGLONG   = 8,
	MYSQL_TYPE_INT24      = 9,
	MYSQL_TYPE_DATE       = 10,
	MYSQL_TYPE_TIME       = 11,
	MYSQL_TYPE_DATETIME   = 12,
	MYSQL_TYPE_YEAR       = 13,
	MYSQL_TYPE_VARCHAR    = 15,
	MYSQL_TYPE_BIT        = 16,
	MYSQL_TYPE_VECTOR     = 242, // MySQL 9.x VECTOR column
	MYSQL_TYPE_JSON       = 245,
	MYSQL_TYPE_NEWDECIMAL = 246,
	MYSQL_TYPE_ENUM       = 247,
	MYSQL_TYPE_SET        = 248,
	MYSQL_TYPE_TINY_BLOB  = 249,
	MYSQL_TYPE_MEDIUM_BLOB= 250,
	MYSQL_TYPE_LONG_BLOB  = 251,
	MYSQL_TYPE_BLOB       = 252,
	MYSQL_TYPE_VAR_STRING = 253,
	MYSQL_TYPE_STRING     = 254,
	MYSQL_TYPE_GEOMETRY   = 255,
}

/// Time unit for mysql_rs_exec_time().
enum E_MYSQL_EXECTIME_UNIT
{
	MICROSECONDS = 0,
	MILLISECONDS,
}

/// Password hashing algorithm for mysql_hash()/mysql_verify().
enum E_MYSQL_HASH_ALGO
{
	HASH_ARGON2ID = 0, // OWASP-recommended, memory-hard. Default.
	HASH_BCRYPT,       // widely used; for compatibility with existing tables.
}

/// Distance metric for VECTOR similarity (mirrors SQL DISTANCE() metrics).
enum E_MYSQL_VECTOR_METRIC
{
	VECTOR_COSINE = 0,
	VECTOR_DOT,
	VECTOR_EUCLIDEAN,
}

/// Per-connection query guard configured via mysql_set_limit().
/// A value of 0 disables that guard (the default for PENDING and RATE; the
/// length cap defaults to 1 MiB until changed).
enum E_MYSQL_LIMIT
{
	LIMIT_MAX_LENGTH = 0,  // reject queries longer than `value` bytes
	LIMIT_MAX_PENDING,     // reject when `value` queries are already queued/in-flight
	LIMIT_RATE_PER_SEC,    // reject more than `value` queries per second
}


/* =============================================================================
 *  Connection management
 * ========================================================================== */

/**
 * Open a TLS-encrypted connection to a MySQL server.
 *
 * The connection is always encrypted; pass an options set (mysql_config_create)
 * to control certificate strictness, port, timeouts, compression, etc.
 *
 * @param  host      server host or IP
 * @param  user      MySQL user
 * @param  password  MySQL password (prefer loading this from a file/env, not
 *                   hard-coded in your .pwn — see mysql_connect_config)
 * @param  database  default schema to USE
 * @param  options   an options set from mysql_config_create(), or
 *                   MYSQL_INVALID_OPTIONS for secure defaults
 * @return a connection handle, or MYSQL_INVALID_HANDLE on failure
 *         (check mysql_errno()/mysql_error()).
 */
native MySQL:mysql_connect(const host[], const user[], const password[], const database[], MySQLConfig:options = MYSQL_INVALID_OPTIONS);

/**
 * Open a connection using settings from an .ini file. Keeps credentials and
 * TLS config out of your source. Supports ${VAR} expansion from environment
 * variables, e.g.  password=${DB_PASS}
 *
 * Recognised keys: host, user, password, database, port, ssl_mode, ssl_ca,
 * ssl_cert, ssl_key, ssl_crl, tls_version, compression, auto_reconnect, pool_size.
 *
 * Debug logging is opt-in from the config too (default OFF):
 *   debug     = 1                      ; enable [omp-MySQL] diagnostics
 *   debug_log = logs/mysql-debug.log   ; (optional) also mirror to this file
 *
 * The file may be plaintext OR an obfuscated blob produced by
 * mysql_config_obfuscate() — it's decoded transparently. (See that native for
 * the honest caveat: obfuscation, not encryption.)
 */
native MySQL:mysql_connect_config(const file_name[] = "mysql.ini");

/**
 * One-time authoring helper: read a plaintext .ini and write an obfuscated blob
 * that mysql_connect_config() reads transparently. Lets you avoid shipping the
 * connection config in plaintext.
 *
 * HONEST CAVEAT: this is OBFUSCATION, not encryption. The algorithm is public
 * (this is open source); only the KEY keeps a blob unreadable. Since client-side
 * crypto can't hide a key from whoever runs the binary, the key is an OPERATOR
 * SECRET you set in the environment as OMP_CFG_KEY — never in the source. A dumped
 * server directory then yields the blob but NOT the env-only key, so it can't be
 * decoded from the files. (If OMP_CFG_KEY is unset, a weak built-in key is used
 * and a warning is logged — barely better than base64.) Real protection comes
 * from ${ENV} secrets + a least-privilege DB user + mandatory TLS; this is only a
 * thin "not plaintext on disk" layer. Run once, then ship only the blob.
 *
 *   # operator sets a per-server secret in the environment, once:
 *   #   export OMP_CFG_KEY="long-random-string-only-i-know"
 *   mysql_config_obfuscate("mysql.plain.ini", "mysql.cfg");  // author once
 *   mysql_connect_config("mysql.cfg");                       // at runtime
 */
native bool:mysql_config_obfuscate(const plain_ini[], const out_file[]);

/// Close a connection and free its resources.
native mysql_close(MySQL:handle = MYSQL_DEFAULT_HANDLE);

/// Number of queries still queued/in-flight on this connection.
native mysql_pending_count(MySQL:handle = MYSQL_DEFAULT_HANDLE);

/**
 * Configure a per-connection query guard (injection / abuse hardening). These
 * are enforced on the main thread before a query is dispatched; a rejected
 * query never executes and (for async queries) reports via OnQueryError.
 *   mysql_set_limit(db, LIMIT_MAX_LENGTH, 4096);   // cap query bytes
 *   mysql_set_limit(db, LIMIT_MAX_PENDING, 256);   // backpressure
 *   mysql_set_limit(db, LIMIT_RATE_PER_SEC, 1000); // sliding-window rate
 * Pass 0 to disable a guard. Stacked-statement queries are ALWAYS rejected
 * unless the connection enabled multi_statements. Returns false on a bad handle.
 */
native mysql_set_limit(MySQL:handle, E_MYSQL_LIMIT:type, value);

/// Set a process-wide option (call before connecting).
native mysql_global_set(E_MYSQL_GLOBAL_OPTION:type, value);

/// Create a fresh, empty connection-options set (secure defaults applied).
native MySQLConfig:mysql_config_create();

/**
 * Set one option on an options set. The value is a single argument: an integer
 * (numeric/bool/enum options) or a string (the SSL_* paths / TLS_VERSION /
 * COMPRESSION). The component detects which from the argument at runtime.
 *   mysql_config_set(opt, SSL_MODE, SSL_MODE_VERIFY_CA);   // int/enum
 *   mysql_config_set(opt, SERVER_PORT, 3307);              // int
 *   mysql_config_set(opt, SSL_CA, "ca.pem");               // string
 */
native mysql_config_set(MySQLConfig:options, E_MYSQL_OPTION:type, {Float,_}:...);


/* =============================================================================
 *  Errors, status, charset
 * ========================================================================== */

/// MySQL error number from the last operation on this connection (0 = OK).
native mysql_errno(MySQL:handle = MYSQL_DEFAULT_HANDLE);

/// Human-readable text for the last error. Returns the length written.
native mysql_error(destination[], maxlength = sizeof(destination), MySQL:handle = MYSQL_DEFAULT_HANDLE);

/// Server status string (uptime, threads, etc.), as `mysqladmin status` shows.
native mysql_server_status(destination[], maxlength = sizeof(destination), MySQL:handle = MYSQL_DEFAULT_HANDLE);

/// Set the connection character set. Defaults to utf8mb4; you rarely need this.
native mysql_set_charset(const charset[], MySQL:handle = MYSQL_DEFAULT_HANDLE);

/// Get the current connection character set.
native mysql_get_charset(destination[], maxlength = sizeof(destination), MySQL:handle = MYSQL_DEFAULT_HANDLE);


/* =============================================================================
 *  Version / update check  (new)
 * ========================================================================== */

/// This build's version string (e.g. "1.0.0").
native mysql_version(destination[], maxlength = sizeof(destination));

/// The latest published version this build is aware of. If it differs from
/// mysql_version(), a newer omp-MySQL has been released — update to stay current.
native mysql_latest_version(destination[], maxlength = sizeof(destination));

/// True if a newer omp-MySQL release than this build is known to be available.
native bool:mysql_update_available();


/* =============================================================================
 *  Debug logging  (new)
 *
 *  Opt-in verbose diagnostics, tagged "[omp-MySQL]". When on, the component
 *  narrates connections, queries, errors and the prepared-statement path to the
 *  console — and, if a file is given, appends timestamped lines there too. Off by
 *  default (zero overhead). Toggle at runtime (e.g. from a /mysql RCON command).
 * ========================================================================== */

/// Enable/disable "[omp-MySQL]" debug logging. Pass a non-empty `logfile` to also
/// mirror debug lines (timestamped) to that server-side file. Returns true.
///   mysql_debug(true);                         // console only
///   mysql_debug(true, "logs/mysql-debug.log"); // console + file
///   mysql_debug(false);                        // off
native mysql_debug(bool:enable, const logfile[] = "");

/// True if debug logging is currently enabled.
native bool:mysql_debug_enabled();

/// Emit your own "[omp-MySQL]"-tagged debug line (no-op unless debug is on). Lets
/// a gamemode funnel its diagnostics through the same console/file sink.
native mysql_log(const message[]);


/* =============================================================================
 *  TLS introspection  (new)
 * ========================================================================== */

/// Negotiated TLS cipher suite name (e.g. "TLS_AES_256_GCM_SHA384"). Empty if,
/// somehow, the link is not encrypted. Use to prove the connection is secure.
native mysql_get_tls_cipher(destination[], maxlength = sizeof(destination), MySQL:handle = MYSQL_DEFAULT_HANDLE);

/// true if the connection is TLS-encrypted (always true by construction).
native bool:mysql_is_tls_enabled(MySQL:handle = MYSQL_DEFAULT_HANDLE);


/* =============================================================================
 *  Escaping & formatting
 *
 *  Prefer prepared statements (mysql_stmt_*) over these. Use these only when
 *  you must build a query string yourself.
 * ========================================================================== */

/// Escape a string so it is safe to place inside single quotes in a query.
native mysql_escape(const source[], destination[], maxlength = sizeof(destination), MySQL:handle = MYSQL_DEFAULT_HANDLE);

/**
 * printf-style query builder. Specifiers:
 *   %s  string            %d / %i  integer        %f  float
 *   %e  escaped string (safe to embed in quotes)
 *   %i  escaped `identifier` (backtick-quoted table/column name)   (new)
 *   %%  a literal percent sign
 * Always use %e for user-supplied values if you are not using bind parameters.
 */
native mysql_format(MySQL:handle, output[], maxlength, const format[], {Float,_}:...);


/* =============================================================================
 *  Queries
 *
 *  - mysql_execute : async, fires a callback when done (most common)
 *  - mysql_execute_for : async, callback bound to a playerid (auto-invalidated on
 *                   disconnect — safe for per-player queries)
 *  - mysql_execute_sync  : SYNCHRONOUS, blocks the server. Use sparingly.
 * ========================================================================== */

/**
 * Run a query asynchronously and call `callback` on completion.
 * Inside the callback the result set is the active cache (use the mysql_rs_* natives).
 *
 * The `format`/`...` pass-through lets you forward extra args to the callback:
 *   mysql_execute(db, "SELECT ...", "OnLoad", "d", playerid);
 *   forward OnLoad(playerid);
 */
native mysql_execute(MySQL:handle, const query[], const callback[] = "", const format[] = "", {Float,_}:...);

/// Like mysql_execute, but the first pass-through arg is a playerid; the callback
/// is cancelled automatically if that player disconnects first.
native mysql_execute_for(MySQL:handle, const query[], const callback[] = "", const format[] = "", {Float,_}:...);

/// Synchronous query. Blocks until done; returns a saved ResultSet: handle (or
/// MYSQL_INVALID_CACHE). You must mysql_rs_release() the result when finished.
native ResultSet:mysql_execute_sync(MySQL:handle, const query[], bool:use_cache = true);

/// Async query whose text is read from a .sql file (supports multiple
/// statements when MULTI_STATEMENTS is enabled on the connection).
native mysql_execute_file(MySQL:handle, const file_path[], const callback[] = "", const format[] = "", {Float,_}:...);

/// Synchronous variant of mysql_execute_file.
native ResultSet:mysql_execute_file_sync(MySQL:handle, const file_path[], bool:use_cache = false);


/* =============================================================================
 *  Prepared statements  (new)  —  the recommended, injection-safe query path
 *
 *  Bind values to ? placeholders; the values can never be interpreted as SQL.
 *  Pattern:
 *      new PreparedStatement:s = mysql_prepare(db, "SELECT id FROM users WHERE name = ?");
 *      mysql_stmt_set_string(s, 0, name);
 *      mysql_stmt_execute(s, "OnFound", "d", playerid);   // async
 *      ...
 *      mysql_stmt_close(s);
 * ========================================================================== */

/// Prepare a statement with `?` placeholders. Returns MYSQL_INVALID_STMT on error.
native PreparedStatement:mysql_prepare(MySQL:handle, const query[]);

/// Bind an integer to placeholder `param_index` (1-based: first ? is 1).
native mysql_stmt_set_int(PreparedStatement:stmt, param_index, value);

/// Bind a float to placeholder `param_index`.
native mysql_stmt_set_float(PreparedStatement:stmt, param_index, Float:value);

/// Bind a string to placeholder `param_index`.
native mysql_stmt_set_string(PreparedStatement:stmt, param_index, const value[]);

/// Bind SQL NULL to placeholder `param_index`.
native mysql_stmt_set_null(PreparedStatement:stmt, param_index);

/**
 * Execute a prepared statement. Async if a callback is given (result becomes
 * the active cache inside it), otherwise runs synchronously.
 * Re-bind and execute again to reuse the statement cheaply.
 */
native mysql_stmt_execute(PreparedStatement:stmt, const callback[] = "", const format[] = "", {Float,_}:...);

/// Free a prepared statement.
native mysql_stmt_close(PreparedStatement:stmt);


/* =============================================================================
 *  Password hashing  (new)  —  Argon2id (default) / bcrypt
 *
 *  Hashing is deliberately slow, so the async forms run on a worker thread and
 *  never block the main server tick. Store the full hash string in your DB; it
 *  embeds the salt and parameters.
 * ========================================================================== */

/**
 * Hash a password (async). Calls `callback(playerid, hash[])`-style; you supply
 * the forward and pass-through args via format/...
 *   mysql_hash(pass, "OnHashed", "d", playerid);
 *   forward OnHashed(playerid, const hash[]);
 */
native mysql_hash(const password[], const callback[], const format[] = "", E_MYSQL_HASH_ALGO:algo = HASH_ARGON2ID, {Float,_}:...);

/// Synchronous hash into `destination`. Returns length written. Prefer the async
/// form in production (this blocks while it computes).
native mysql_hash_sync(const password[], destination[], maxlength = sizeof(destination), E_MYSQL_HASH_ALGO:algo = HASH_ARGON2ID);

/**
 * Verify a password against a stored hash (async, constant-time).
 *   mysql_verify(pass, storedHash, "OnVerified", "d", playerid);
 *   forward OnVerified(playerid, bool:success);
 * The algorithm is auto-detected from the hash string.
 */
native mysql_verify(const password[], const hash[], const callback[], const format[] = "", {Float,_}:...);

/// Synchronous, constant-time verify. Returns true on match.
native bool:mysql_verify_sync(const password[], const hash[]);


/* =============================================================================
 *  VECTOR helpers  (new, MySQL 9.x)  —  vector similarity search
 *
 *  Store embeddings in a VECTOR column. Convert to/from the textual form
 *  ("[0.1,0.2,...]") that STRING_TO_VECTOR()/VECTOR_TO_STRING() use, then run
 *  similarity with DISTANCE(col, STRING_TO_VECTOR('[...]'), 'COSINE').
 * ========================================================================== */

/// Pack a float array into the "[x,y,z]" textual vector form for STRING_TO_VECTOR.
native mysql_vector_to_string(const Float:values[], count, destination[], maxlength = sizeof(destination));

/// Parse a "[x,y,z]" textual vector (e.g. from VECTOR_TO_STRING) into floats.
/// Returns the number of components written.
native mysql_string_to_vector(const source[], Float:destination[], maxlength = sizeof(destination));

/// Number of components a textual vector string contains (VECTOR_DIM).
native mysql_vector_dim(const source[]);


/* =============================================================================
 *  Result cache  —  reading rows from the active result set
 *
 *  Inside a query callback the result is "active"; the mysql_rs_* natives read it.
 *  To keep a result beyond the callback, mysql_rs_retain() it (and mysql_rs_release()
 *  it later). A query with several statements yields multiple result sets;
 *  switch between them with mysql_rs_select_set().
 * ========================================================================== */

/// Rows in the current result set (also: mysql_rs_num_rows()).
native mysql_rs_row_count(&destination);
/// Columns in the current result set (also: mysql_rs_num_fields()).
native mysql_rs_field_count(&destination);
/// Number of result sets produced by the query (also: mysql_rs_num_sets()).
native mysql_rs_set_count(&destination);

/// Name of column `field_index`.
native mysql_rs_field_name(field_index, destination[], maxlength = sizeof(destination));
/// Declared type of column `field_index`.
native E_MYSQL_FIELD_TYPE:mysql_rs_field_type(field_index);
/// Switch the active result set (for multi-statement queries).
native mysql_rs_select_set(result_index);

stock mysql_rs_num_rows()    { new n; mysql_rs_row_count(n);    return n; }
stock mysql_rs_num_fields()  { new n; mysql_rs_field_count(n);  return n; }
stock mysql_rs_num_sets() { new n; mysql_rs_set_count(n); return n; }

// --- value getters: by column index ---
native mysql_rs_get_string(row, column, destination[], maxlength = sizeof(destination));
native mysql_rs_get_int(row, column, &destination);
native mysql_rs_get_float(row, column, &Float:destination);
native mysql_rs_get_bool(row, column, &bool:destination);
native mysql_rs_is_null(row, column, &bool:destination);

// --- value getters: by column name ---
native mysql_rs_get_string_by(row, const column[], destination[], maxlength = sizeof(destination));
native mysql_rs_get_int_by(row, const column[], &destination);
native mysql_rs_get_float_by(row, const column[], &Float:destination);
native mysql_rs_get_bool_by(row, const column[], &bool:destination);
native mysql_rs_is_null_by(row, const column[], &bool:destination);

/*
 * Convenience aliases. The unsuffixed mysql_rs_value* names read by COLUMN
 * NAME — the common, readable case:
 *     mysql_rs_value_int(row, "score", score);
 * To read by numeric column index instead, call the *_index variants above:
 *     mysql_rs_get_int(row, 2, score);
 *
 * (We intentionally don't ship one macro that guesses name-vs-index from the
 * argument: in Pawn that forces both a by-name and a by-index native call to
 * type-check at every call site, which the compiler rejects.)
 */
#define mysql_rs_value       mysql_rs_get_string_by
#define mysql_rs_value_int   mysql_rs_get_int_by
#define mysql_rs_value_float mysql_rs_get_float_by
#define mysql_rs_value_bool  mysql_rs_get_bool_by
#define mysql_rs_value_is_null   mysql_rs_is_null_by

// --- saving / activating result sets ---
/// Persist the active result set so it survives past the callback. Returns a
/// ResultSet: handle you must later mysql_rs_release().
native ResultSet:mysql_rs_retain();
/// Free a saved result set.
native mysql_rs_release(ResultSet:rs_id);
/// Make a saved result set the active one for subsequent mysql_rs_* reads.
native mysql_rs_activate(ResultSet:rs_id);
/// Clear the active result set.
native mysql_rs_deactivate();
/// true if any result set is currently active.
native bool:mysql_rs_is_active();
/// true if `rs_id` refers to a valid saved result set.
native bool:mysql_rs_is_valid(ResultSet:rs_id);

// --- per-query metadata ---
/// Rows changed by the last INSERT/UPDATE/DELETE.
native mysql_rs_affected_rows();
/// AUTO_INCREMENT id generated by the last INSERT.
native mysql_rs_insert_id();
/// Warnings raised by the last statement.
native mysql_rs_warning_count();
/// How long the query took to execute on the server.
native mysql_rs_exec_time(E_MYSQL_EXECTIME_UNIT:unit = MICROSECONDS);
/// The exact query text that produced this result.
native mysql_rs_query(destination[], maxlength = sizeof(destination));


/* =============================================================================
 *  Model  —  active-record mapping of a table row to Pawn variables
 *
 *  An optional convenience layer (think ActiveRecord / a tiny ORM). Bind your
 *  Pawn variables to columns once, set the primary key, then find / save /
 *  delete a single row without writing SQL. All bound variables must be
 *  GLOBALS — the model keeps references to them across async calls and writes
 *  results back into them when a find completes.
 *
 *  For bulk work or complex queries, prepared statements (mysql_prepare /
 *  mysql_stmt_*) are clearer and faster.
 * ========================================================================== */

/// Create a model bound to `table` on a connection. Returns a Model: handle
/// (MYSQL_INVALID_MODEL on failure).
native Model:mysql_model_create(const table[], MySQL:handle = MYSQL_DEFAULT_HANDLE);

/// Destroy a model and release its handle.
native mysql_model_destroy(Model:model);

/// Result of the model's last operation (see E_MYSQL_MODEL_ERROR).
native E_MYSQL_MODEL_ERROR:mysql_model_errno(Model:model);

/// Bind a Pawn variable to a column. The variable is read on save/insert/update
/// and written on find. Bind globals only.
native mysql_model_bind_int(Model:model, &variable, const column[]);
native mysql_model_bind_float(Model:model, &Float:variable, const column[]);
native mysql_model_bind_string(Model:model, variable[], variable_maxlen, const column[]);

/// Remove a single column binding, or clear them all.
native mysql_model_unbind(Model:model, const column[]);
native mysql_model_clear(Model:model);

/// Set the primary-key column (used by find / save / delete to locate the row).
native mysql_model_set_key(Model:model, const column[]);

/// SELECT the row whose key column equals the bound key variable, and write the
/// result into the bound variables. (Async; callback fired when done.)
native mysql_model_find(Model:model, const callback[] = "", const format[] = "", {Float,_}:...);

/// INSERT a new row from the bound variables. The auto-increment key (if any) is
/// written back into the bound key variable.
native mysql_model_insert(Model:model, const callback[] = "", const format[] = "", {Float,_}:...);

/// UPDATE the keyed row from the bound variables.
native mysql_model_update(Model:model, const callback[] = "", const format[] = "", {Float,_}:...);

/// Upsert: INSERT if the key is unset/zero, otherwise UPDATE the keyed row.
native mysql_model_save(Model:model, const callback[] = "", const format[] = "", {Float,_}:...);

/// DELETE the keyed row.
native mysql_model_delete(Model:model, const callback[] = "", const format[] = "", {Float,_}:...);


/* =============================================================================
 *  Callbacks  (forwards)
 * ========================================================================== */

/**
 * Fired when a query fails. Return 1 to suppress the default error log.
 * Never echo `query`/`error` into public chat — they may contain sensitive data.
 *
 * @param errorid   MySQL error number (see the ER_ and CR_ constants)
 * @param error     error text
 * @param callback  the callback the failed query was going to call
 * @param query     the query text (credentials are already redacted)
 * @param handle    the connection it ran on
 */
forward OnQueryError(errorid, const error[], const callback[], const query[], MySQL:handle);

/**
 * Optional audit hook fired for every successfully executed query. Enable it by
 * defining MYSQL_AUDIT before including this file. Useful for forensic logging.
 *
 * @param handle     the connection
 * @param query      the executed query (redacted)
 * @param exec_time  server execution time, microseconds
 */
#if defined MYSQL_AUDIT
forward OnQueryExecute(MySQL:handle, const query[], exec_time);
#endif


/* =============================================================================
 *  Deprecated shims  —  keep old gamemodes compiling; do nothing useful.
 * ========================================================================== */

/// Old log-level flags. Logging is now configured in log-config.yml; this enum
/// is kept only so legacy code referencing LOG_* still compiles. Guarded in
/// case another include already defines E_LOGLEVEL.
#if !defined E_LOGLEVEL
enum E_LOGLEVEL
{
	LOG_NONE    = 0,
	LOG_DEBUG   = 1,
	LOG_INFO    = 2,
	LOG_WARNING = 4,
	LOG_ERROR   = 8,
	LOG_ALL     = LOG_DEBUG | LOG_INFO | LOG_WARNING | LOG_ERROR,
}
#endif

/*
 * NOTE: mysql_log(level) is intentionally NOT provided. It used to set logging
 * verbosity from Pawn; logging is now configured entirely in log-config.yml.
 * Porting an old gamemode? Just delete your mysql_log(...) calls. (We don't ship
 * a no-op shim: a never-called compat function warns "unused symbol" on the
 * open.mp Pawn compiler.)
 */
