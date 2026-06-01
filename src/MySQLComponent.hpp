/* =========================================
 *
 *  MySQL for open.mp  —  component class
 *  ------------------------------------------
 *
 *  Clean-room implementation. See NOTICE.md.
 *  Author: Xyranaut (Mac Andreas)  ·  License: MIT (see LICENSE)
 *
 * ========================================= */

#pragma once

#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>

#include "Connection.hpp"
#include "QueryQueue.hpp"
#include "Query.hpp"
#include "HashJob.hpp"
#include "Model.hpp"

#include <memory>
#include <unordered_map>
#include <deque>
#include <chrono>
#include <string>

using namespace Impl;

// Per-connection query guards (injection / abuse hardening). These are policy
// limits enforced on the MAIN thread in enqueueJob, before a job ever reaches a
// worker — a rejected query is never executed.
struct QueryGuards
{
	// Reject a query whose SQL exceeds this many bytes. 0 = no limit.
	std::size_t maxQueryLength = 1u * 1024u * 1024u; // 1 MiB

	// Reject when more than `maxQueriesPerWindow` queries have been enqueued on a
	// connection within `rateWindow`. 0 = no rate limit.
	unsigned int maxQueriesPerWindow = 0; // off by default; opt-in
	std::chrono::milliseconds rateWindow { 1000 };

	// Reject when the worker already has at least this many jobs queued/in-flight
	// (backpressure against runaway producers). 0 = no limit.
	int maxPendingQueries = 0; // off by default; opt-in
};

/// The MySQL database component. Owns the connection manager + async engine and
/// registers the mysql_* (incl. mysql_rs_* and mysql_model_*) natives with every loaded Pawn script.
class MySQLComponent final
	: public IComponent
	, public PawnEventHandler
	, public CoreEventHandler
{
private:
	ICore* core_ = nullptr;
	IPawnComponent* pawn_ = nullptr;

	std::unordered_map<int, std::unique_ptr<Connection>> connections_;
	// Each connection has a dedicated worker thread; the worker holds the
	// Connection* and is stopped/destroyed BEFORE the Connection it drives.
	std::unordered_map<int, std::unique_ptr<ConnectionWorker>> workers_;
	int nextHandle_ = 1;

	std::unordered_map<int, ConnectionOptions> options_;
	int nextOptions_ = 1;

	// --- Per-connection query guards / policy --------------------------------
	// Whether this connection negotiated CLIENT_MULTI_STATEMENTS at connect time.
	// When false (the default), enqueueJob rejects any query that stacks more
	// than one statement — the injection-stacking defense.
	std::unordered_map<int, bool> multiStatementAllowed_;
	std::unordered_map<int, QueryGuards> guards_;
	// Recent enqueue timestamps per connection, for the sliding-window rate limit.
	std::unordered_map<int, std::deque<std::chrono::steady_clock::time_point>> rateLog_;

	QueryEngine engine_;

	// --- Result cache state --------------------------------------------------
	// The "active" cache is what cache_* natives read; it's set just before a
	// query callback runs and cleared after. cache_save() copies the active
	// result into savedCaches_ for later cache_set_active() use.
	std::shared_ptr<QueryResult> activeCache_;
	int activeResultIndex_ = 0; // which result set in a multi-statement result
	std::unordered_map<int, std::shared_ptr<QueryResult>> savedCaches_;
	int nextCache_ = 1;

	// Prepared-statement registry (1-based handles; 0 == MYSQL_INVALID_STMT).
	std::unordered_map<int, std::unique_ptr<PreparedStatement>> statements_;
	int nextStatement_ = 1;

	// Model registry (1-based handles; 0 == MYSQL_INVALID_MODEL).
	std::unordered_map<int, std::unique_ptr<Model>> models_;
	int nextModel_ = 1;

	// --- Debug logging state -------------------------------------------------
	bool debug_ = false;
	std::string debugFile_; // empty == console only

	inline static MySQLComponent* instance_ = nullptr;

public:
	// --- Version / update check ----------------------------------------------
	// This build's version and the latest version it knows to exist. LATEST is
	// bumped when a release is published, so a server running an older DLL learns
	// (in the server log on load, and via mysql_update_available()) that an update
	// is out. Single source of truth for the load banner + the version natives.
	static constexpr const char* kVersion = "1.0.0";
	static constexpr const char* kLatestVersion = "1.0.0";
	static bool updateAvailable()
	{
		return std::string(kVersion) != std::string(kLatestVersion);
	}

private:

public:
	// Unique component id ("ompmysql" packed into a UID).
	PROVIDE_UID(UID(0x6F6D706D7973716C));

	static MySQLComponent* getInstance();

	ICore* getCore() const { return core_; }
	IPawnComponent* pawn() const { return pawn_; }

	/// Find the loaded Pawn script (main OR a filterscript) that defines `publicName`,
	/// returning its AMX (or nullptr). Async callbacks (mysql_execute/stmt/model)
	/// live in whichever script issued the query — often a filterscript, not the
	/// gamemode — so dispatch must search all scripts, not just mainScript().
	AMX* findScriptWithPublic(const char* publicName);

	/// Log a line through open.mp's core logger (prefixed "[MySQL]").
	void log(LogLevel level, StringView message);

	// --- Debug logging -------------------------------------------------------
	// Opt-in verbose diagnostics, tagged "[omp-MySQL]". When enabled, the
	// component narrates connections, queries, errors and the prepared-statement
	// path to the console and (optionally) to a server-side log file. Toggled at
	// runtime via the mysql_debug() native / the filterscript's /mysql command.

	/// Enable/disable debug logging. `file` (optional) also mirrors debug lines to
	/// that path (relative to the server dir), appended with timestamps.
	void setDebug(bool on, StringView file = StringView());
	bool debugEnabled() const { return debug_; }
	const std::string& debugFile() const { return debugFile_; }

	/// Emit a debug line (no-op unless debug is on). Always tagged "[omp-MySQL]".
	void debugLog(StringView message);

	// --- Connection registry -------------------------------------------------
	// Handles are small positive ints (1..N); 0 == MYSQL_INVALID_HANDLE.
	// Stores own the Connection objects.

	/// Register a new connection, returning its 1-based handle (0 on failure).
	/// `multiStatements` records whether the connection negotiated stacked
	/// statements; when false, enqueueJob rejects multi-statement SQL.
	int addConnection(std::unique_ptr<Connection> conn, bool multiStatements = false);

	/// Look up a connection by handle, or nullptr if unknown/invalid.
	Connection* getConnection(int handle);

	/// Reverse lookup: the handle owning `conn`, or 0 if not found.
	int connectionHandleOf(const Connection* conn) const;

	/// Close and remove a connection. Returns true if it existed.
	bool removeConnection(int handle);

	/// Drop all connections (used on free/reset).
	void clearConnections();

	// --- Async query engine --------------------------------------------------

	/// Why enqueueJob refused a query (for OnQueryError reporting / logging).
	enum class GuardReject
	{
		None = 0,
		UnknownHandle,
		TooLong,
		RateLimited,
		TooManyPending,
		MultiStatement,
	};

	/// Enqueue a job on its connection's worker. Returns false if the handle is
	/// unknown OR a guard rejected the query (the job is dropped). Takes
	/// ownership of `job`. On rejection, `*reason` (if non-null) says why and the
	/// OnQueryExecute audit hook is NOT fired.
	bool enqueueJob(std::unique_ptr<QueryJob> job, GuardReject* reason = nullptr);

	/// Inspect / configure the guards for a connection (no-op if unknown handle).
	QueryGuards* guardsFor(int handle);

	/// Whether `handle` negotiated CLIENT_MULTI_STATEMENTS (false if unknown).
	bool multiStatementsAllowed(int handle) const;

	/// True if `sql` contains more than one statement (a `;` separating non-empty,
	/// non-comment SQL), honouring quotes and comments. Used for the
	/// multi-statement-off defense.
	static bool isMultiStatement(const std::string& sql);

	/// Jobs queued or in-flight on a connection's worker (0 if unknown).
	int pendingQueries(int handle) const;

	/// Fire the Pawn callback (or OnQueryError) for one finished job.
	void dispatchJob(const QueryJob& job);

	/// Fire the OnQueryExecute(MySQL:handle, const query[], exec_time) audit hook
	/// on the main script after a query has run (exec_time = measured server
	/// execution time, microseconds). Best-effort: no-ops if the optional forward
	/// (gated by MYSQL_AUDIT in the script) is absent.
	void fireQueryExecuteHook(int handle, const std::string& sql, long long execMicros);

	// --- Options-set registry ------------------------------------------------
	// A scratch ConnectionOptions a script builds with mysql_set_option before
	// passing the handle to mysql_connect. 1-based; 0 == MYSQL_INVALID_OPTIONS.

	/// Create a new options set (secure defaults), returning its handle.
	int addOptions();

	/// Look up an options set, or nullptr if unknown.
	ConnectionOptions* getOptions(int handle);

	/// Drop all options sets.
	void clearOptions();

	// --- Prepared statements -------------------------------------------------

	/// Register a prepared statement; returns its 1-based handle (0 on failure).
	int addStatement(std::unique_ptr<PreparedStatement> stmt);
	/// Look up a statement, or nullptr if unknown.
	PreparedStatement* getStatement(int handle);
	/// Free a statement. Returns true if it existed.
	bool removeStatement(int handle);
	void clearStatements();

	// --- Models (active-record layer) ----------------------------------------

	/// Register a model; returns its 1-based handle (0 on failure).
	int addModel(std::unique_ptr<Model> model);
	/// Look up a model, or nullptr if unknown.
	Model* getModel(int handle);
	/// Free a model. Returns true if it existed.
	bool removeModel(int handle);
	void clearModels();

	/// Write a finished model job's result back into its bound Pawn variables
	/// (find → row values; insert → auto-increment id into the key). Main thread.
	/// Declared here (impl in MySQLComponent.cpp) because it needs AMX access.
	void applyModelResult(AMX* amx, const QueryJob& job);

	// --- Password hashing (async, off the main thread) -----------------------

	/// Enqueue an Argon2id hash/verify job (dispatched on a later tick).
	void enqueueHash(std::unique_ptr<HashJob> job);
	/// Fire a finished hash/verify job's Pawn callback (main thread).
	void dispatchHash(const HashJob& job);

	// --- Result cache (read by the cache_* natives) --------------------------

	/// The active result set, or nullptr if none is active.
	const ResultSet* activeSet() const;

	/// Set/clear the active cache (used around callback dispatch).
	void setActiveCache(std::shared_ptr<QueryResult> result, int resultIndex = 0);
	void clearActiveCache();
	bool hasActiveCache() const { return activeCache_ != nullptr; }

	/// Select which result set within the active multi-statement result is read.
	bool setActiveResultIndex(int index);
	int activeResultIndex() const { return activeResultIndex_; }
	const QueryResult* activeResult() const { return activeCache_.get(); }

	/// Persist the active result; returns its saved-cache handle (0 if none).
	int saveActiveCache();
	/// Activate / drop / validate a saved cache.
	bool activateSavedCache(int cacheId);
	bool deleteSavedCache(int cacheId);
	bool isValidCache(int cacheId) const;

	// --- IComponent ---
	StringView componentName() const override { return "MySQL"; }
	SemanticVersion componentVersion() const override { return SemanticVersion(0, 1, 0, 0); }
	void onLoad(ICore* c) override;
	void onInit(IComponentList* components) override;
	void onReady() override;
	void onFree(IComponent* component) override;
	void free() override;
	void reset() override;

	// --- PawnEventHandler ---
	void onAmxLoad(IPawnScript& script) override;
	void onAmxUnload(IPawnScript& script) override;

	// --- CoreEventHandler ---
	// Drains finished async queries each tick and fires their callbacks on the
	// main thread (the only place it's safe to touch the AMX).
	void onTick(Microseconds elapsed, TimePoint now) override;

	~MySQLComponent();
};
