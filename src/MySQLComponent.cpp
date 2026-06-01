/* =========================================
 *
 *  MySQL for open.mp  —  component implementation
 *
 *  Clean-room. See NOTICE.md. Author: Xyranaut (Mac Andreas). MIT.
 *
 * ========================================= */

#include "MySQLComponent.hpp"
#include "Connection.hpp"
#include "Statement.hpp"

#include <Server/Components/Pawn/Impl/pawn_natives.hpp>
#include <Server/Components/Pawn/Impl/pawn_impl.hpp>

#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

MySQLComponent* MySQLComponent::getInstance()
{
	if (instance_ == nullptr)
	{
		instance_ = new MySQLComponent();
	}
	return instance_;
}

void MySQLComponent::log(LogLevel level, StringView message)
{
	if (core_ == nullptr)
	{
		return;
	}
	std::string line = "[MySQL] ";
	line.append(message.data(), message.length());
	core_->logLn(level, "%.*s", PRINT_VIEW(StringView(line)));
}

void MySQLComponent::setDebug(bool on, StringView file)
{
	debug_ = on;
	debugFile_.assign(file.data(), file.length());
	if (on)
	{
		std::string msg = "debug logging ENABLED";
		if (!debugFile_.empty())
		{
			msg += " (mirroring to '" + debugFile_ + "')";
		}
		debugLog(StringView(msg));
	}
}

void MySQLComponent::debugLog(StringView message)
{
	if (!debug_)
	{
		return;
	}
	std::string body(message.data(), message.length());

	// Console (open.mp logger), tagged so it's grep-able and distinct from the
	// component's normal "[MySQL]" lines.
	if (core_ != nullptr)
	{
		std::string line = "[omp-MySQL] " + body;
		core_->logLn(LogLevel::Debug, "%.*s", PRINT_VIEW(StringView(line)));
	}

	// Optional server-side file (timestamped, appended). Best-effort; a file we
	// can't open just means console-only — never throws into the caller.
	if (!debugFile_.empty())
	{
		if (std::FILE* f = std::fopen(debugFile_.c_str(), "ab"))
		{
			std::time_t t = std::time(nullptr);
			char ts[32];
			std::tm tmv {};
#if defined(_WIN32)
			localtime_s(&tmv, &t);
#else
			localtime_r(&t, &tmv);
#endif
			std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
			std::fprintf(f, "[%s] [omp-MySQL] %s\n", ts, body.c_str());
			std::fclose(f);
		}
	}
}

void MySQLComponent::onLoad(ICore* c)
{
	core_ = c;
	// Tick events drive async result dispatch on the main thread.
	core_->getEventDispatcher().addEventHandler(this);
	// Spin up the password-hashing thread pool (Argon2id is intentionally slow).
	engine_.startHashing(2);
	std::string banner = std::string("  omp-mySQL v") + kVersion
		+ " loaded (clean-room; MariaDB Connector/C 3.4.8, modern MySQL 8.x/9.x).";
	core_->printLn("%.*s", PRINT_VIEW(StringView(banner)));

	// Operator-visible update notice in the server log (not just to in-game admins).
	if (updateAvailable())
	{
		std::string upd = std::string("  [omp-MySQL]: New version available ") + kLatestVersion
			+ " (current: " + kVersion + "). Update to keep your server secure.";
		core_->logLn(LogLevel::Warning, "%.*s", PRINT_VIEW(StringView(upd)));
	}
}

void MySQLComponent::enqueueHash(std::unique_ptr<HashJob> job)
{
	engine_.enqueueHash(std::move(job));
}

void MySQLComponent::onInit(IComponentList* components)
{
	// Pawn wiring: needed to register natives and forward callbacks.
	pawn_ = components->queryComponent<IPawnComponent>();
	if (pawn_ != nullptr)
	{
		setAmxFunctions(pawn_->getAmxFunctions());
		setAmxLookups(components);
		pawn_->getEventDispatcher().addEventHandler(this);
	}
	else
	{
		log(LogLevel::Error, "Pawn component not found; mysql_* natives unavailable.");
	}
}

void MySQLComponent::onReady()
{
	// Other components are ready here; nothing to do yet (Phase 3+).
}

// --- Connection registry ---------------------------------------------------

int MySQLComponent::addConnection(std::unique_ptr<Connection> conn, bool multiStatements)
{
	if (!conn)
	{
		return 0;
	}
	int handle = nextHandle_++;
	Connection* raw = conn.get();
	connections_.emplace(handle, std::move(conn));
	// Give the connection its dedicated worker thread.
	workers_.emplace(handle, std::make_unique<ConnectionWorker>(
								 raw, engine_.completedSink(), engine_.completedMutex()));
	// Record the query-guard policy for this handle.
	multiStatementAllowed_[handle] = multiStatements;
	guards_[handle] = QueryGuards {};
	return handle;
}

Connection* MySQLComponent::getConnection(int handle)
{
	auto it = connections_.find(handle);
	return it != connections_.end() ? it->second.get() : nullptr;
}

int MySQLComponent::connectionHandleOf(const Connection* conn) const
{
	if (conn == nullptr)
	{
		return 0;
	}
	for (const auto& [handle, owned] : connections_)
	{
		if (owned.get() == conn)
		{
			return handle;
		}
	}
	return 0;
}

bool MySQLComponent::removeConnection(int handle)
{
	// Stop the worker (joins the thread) before destroying its Connection.
	auto w = workers_.find(handle);
	if (w != workers_.end())
	{
		w->second->stop();
		workers_.erase(w);
	}
	multiStatementAllowed_.erase(handle);
	guards_.erase(handle);
	rateLog_.erase(handle);
	return connections_.erase(handle) > 0;
}

void MySQLComponent::clearConnections()
{
	// Workers first (they hold Connection*), then the connections.
	for (auto& kv : workers_)
	{
		kv.second->stop();
	}
	workers_.clear();
	connections_.clear();
	multiStatementAllowed_.clear();
	guards_.clear();
	rateLog_.clear();
}

// --- Async query engine ----------------------------------------------------

bool MySQLComponent::enqueueJob(std::unique_ptr<QueryJob> job, GuardReject* reason)
{
	auto setReason = [&](GuardReject r) {
		if (reason != nullptr)
		{
			*reason = r;
		}
	};

	const int handle = job->connectionHandle;
	auto it = workers_.find(handle);
	if (it == workers_.end())
	{
		setReason(GuardReject::UnknownHandle);
		return false;
	}

	const QueryGuards& g = guards_[handle]; // default-constructed if absent

	// (1) Length cap. Statement-execute jobs carry no SQL here (it lives in the
	// PreparedStatement), so only check plain query jobs.
	if (job->statement == nullptr && g.maxQueryLength != 0
		&& job->sql.size() > g.maxQueryLength)
	{
		setReason(GuardReject::TooLong);
		std::string msg = "query rejected: length " + std::to_string(job->sql.size())
			+ " exceeds limit " + std::to_string(g.maxQueryLength);
		log(LogLevel::Warning, StringView(msg));
		return false;
	}

	// (2) Multi-statement-off enforcement. A prepared statement is single by
	// construction; only guard plain query strings.
	if (job->statement == nullptr)
	{
		auto allowed = multiStatementAllowed_.find(handle);
		bool multiOk = allowed != multiStatementAllowed_.end() && allowed->second;
		if (!multiOk && isMultiStatement(job->sql))
		{
			setReason(GuardReject::MultiStatement);
			log(LogLevel::Warning,
				"query rejected: multiple statements on a connection that did not "
				"enable CLIENT_MULTI_STATEMENTS (possible SQL injection)");
			return false;
		}
	}

	// (3) Pending-queue backpressure.
	if (g.maxPendingQueries != 0 && it->second->pending() >= g.maxPendingQueries)
	{
		setReason(GuardReject::TooManyPending);
		log(LogLevel::Warning, "query rejected: too many pending queries");
		return false;
	}

	// (4) Sliding-window rate limit.
	if (g.maxQueriesPerWindow != 0)
	{
		auto now = std::chrono::steady_clock::now();
		auto& log_ = rateLog_[handle];
		const auto cutoff = now - g.rateWindow;
		while (!log_.empty() && log_.front() < cutoff)
		{
			log_.pop_front();
		}
		if (log_.size() >= g.maxQueriesPerWindow)
		{
			setReason(GuardReject::RateLimited);
			log(LogLevel::Warning, "query rejected: rate limit exceeded");
			return false;
		}
		log_.push_back(now);
	}

	setReason(GuardReject::None);
	it->second->enqueue(std::move(job));
	return true;
}

QueryGuards* MySQLComponent::guardsFor(int handle)
{
	auto it = guards_.find(handle);
	return it != guards_.end() ? &it->second : nullptr;
}

bool MySQLComponent::multiStatementsAllowed(int handle) const
{
	auto it = multiStatementAllowed_.find(handle);
	return it != multiStatementAllowed_.end() && it->second;
}

int MySQLComponent::pendingQueries(int handle) const
{
	auto it = workers_.find(handle);
	return it != workers_.end() ? it->second->pending() : 0;
}

// Scan `sql` once, tracking lexical state, and report whether a statement
// separator (`;`) is followed by more non-trivial SQL. Quotes ('..', "..",
// `..`) and comments (-- … \n, # … \n, /* … */) are skipped so a `;` inside
// them never counts; backslash escapes inside quotes are honoured. A trailing
// `;` (only whitespace/comments after it) is NOT multi-statement.
bool MySQLComponent::isMultiStatement(const std::string& sql)
{
	enum class S { Normal, SQuote, DQuote, BQuote, LineComment, BlockComment };
	S state = S::Normal;
	bool sawSeparator = false;

	for (std::size_t i = 0; i < sql.size(); ++i)
	{
		char c = sql[i];
		char n = (i + 1 < sql.size()) ? sql[i + 1] : '\0';

		switch (state)
		{
		case S::Normal:
			if (c == '\'') { state = S::SQuote; }
			else if (c == '"') { state = S::DQuote; }
			else if (c == '`') { state = S::BQuote; }
			else if (c == '-' && n == '-') { state = S::LineComment; ++i; }
			else if (c == '#') { state = S::LineComment; }
			else if (c == '/' && n == '*') { state = S::BlockComment; ++i; }
			else if (c == ';') { sawSeparator = true; }
			else if (sawSeparator && !std::isspace(static_cast<unsigned char>(c)))
			{
				// Real SQL after a separator -> a second statement.
				return true;
			}
			break;
		case S::SQuote:
			if (c == '\\') { ++i; } // skip escaped char
			else if (c == '\'') { state = S::Normal; }
			break;
		case S::DQuote:
			if (c == '\\') { ++i; }
			else if (c == '"') { state = S::Normal; }
			break;
		case S::BQuote:
			if (c == '`') { state = S::Normal; }
			break;
		case S::LineComment:
			if (c == '\n') { state = S::Normal; }
			break;
		case S::BlockComment:
			if (c == '*' && n == '/') { state = S::Normal; ++i; }
			break;
		}
	}
	return false;
}

AMX* MySQLComponent::findScriptWithPublic(const char* publicName)
{
	if (pawn_ == nullptr)
	{
		return nullptr;
	}
	int idx = -1;
	// Main gamemode first, then every filterscript. The callback for an async
	// query lives in the script that issued it — usually a filterscript here.
	if (IPawnScript* main = pawn_->mainScript())
	{
		if (AMX* amx = main->GetAMX())
		{
			if (amx_FindPublic(amx, publicName, &idx) == AMX_ERR_NONE)
			{
				return amx;
			}
		}
	}
	for (IPawnScript* side : pawn_->sideScripts())
	{
		if (side == nullptr)
		{
			continue;
		}
		if (AMX* amx = side->GetAMX())
		{
			if (amx_FindPublic(amx, publicName, &idx) == AMX_ERR_NONE)
			{
				return amx;
			}
		}
	}
	return nullptr;
}

void MySQLComponent::fireQueryExecuteHook(int handle, const std::string& sql,
	long long execMicros)
{
	if (pawn_ == nullptr)
	{
		return;
	}
	AMX* amx = findScriptWithPublic("OnQueryExecute");
	if (amx == nullptr)
	{
		return; // optional hook (script defines it only under MYSQL_AUDIT)
	}
	int idx = -1;
	if (amx_FindPublic(amx, "OnQueryExecute", &idx) != AMX_ERR_NONE)
	{
		return;
	}
	// forward OnQueryExecute(MySQL:handle, const query[], exec_time)
	// push in REVERSE: exec_time, query, handle.
	cell amxQuery = 0;
	cell* phys = nullptr;
	amx_PushString(amx, &amxQuery, &phys, sql.c_str(), 0, 0);
	amx_Push(amx, static_cast<cell>(execMicros));
	amx_Push(amx, static_cast<cell>(handle));
	cell ret = 0;
	amx_Exec(amx, &ret, idx);
	amx_Release(amx, amxQuery);
}

// --- Options-set registry --------------------------------------------------

int MySQLComponent::addOptions()
{
	int handle = nextOptions_++;
	options_.emplace(handle, ConnectionOptions {});
	return handle;
}

ConnectionOptions* MySQLComponent::getOptions(int handle)
{
	auto it = options_.find(handle);
	return it != options_.end() ? &it->second : nullptr;
}

void MySQLComponent::clearOptions()
{
	options_.clear();
}

// --- Prepared statements ---------------------------------------------------

int MySQLComponent::addStatement(std::unique_ptr<PreparedStatement> stmt)
{
	if (!stmt)
	{
		return 0;
	}
	int handle = nextStatement_++;
	statements_.emplace(handle, std::move(stmt));
	return handle;
}

PreparedStatement* MySQLComponent::getStatement(int handle)
{
	auto it = statements_.find(handle);
	return it != statements_.end() ? it->second.get() : nullptr;
}

bool MySQLComponent::removeStatement(int handle)
{
	auto it = statements_.find(handle);
	if (it == statements_.end())
	{
		return false;
	}
	// The statement's MYSQL_STMT* was created on (and is owned by) the connection's
	// worker thread, so mysql_stmt_close must run THERE, not on this main thread.
	// Hand ownership to a teardown job on that worker. If the worker is already
	// gone (its connection was removed), destroying inline is safe — the MYSQL* is
	// dead and the destructor's stmt_ either never prepared or is moot.
	std::unique_ptr<PreparedStatement> stmt = std::move(it->second);
	statements_.erase(it);

	int connHandle = connectionHandleOf(stmt->connection());
	auto w = (connHandle != 0) ? workers_.find(connHandle) : workers_.end();
	if (w != workers_.end())
	{
		auto job = std::make_unique<QueryJob>();
		job->connectionHandle = connHandle;
		job->statementToClose = std::move(stmt);
		w->second->enqueue(std::move(job));
	}
	// else: worker gone -> stmt destroyed here as it leaves scope.
	return true;
}

void MySQLComponent::clearStatements()
{
	// Tear each statement down on its owning worker (see removeStatement). Drain
	// via the worker so mysql_stmt_close stays off the main thread; any whose
	// worker is gone are destroyed inline as the map clears.
	std::vector<int> handles;
	handles.reserve(statements_.size());
	for (const auto& kv : statements_)
	{
		handles.push_back(kv.first);
	}
	for (int h : handles)
	{
		removeStatement(h);
	}
	statements_.clear();
}

int MySQLComponent::addModel(std::unique_ptr<Model> model)
{
	if (!model)
	{
		return 0;
	}
	int handle = nextModel_++;
	models_.emplace(handle, std::move(model));
	return handle;
}

Model* MySQLComponent::getModel(int handle)
{
	auto it = models_.find(handle);
	return it != models_.end() ? it->second.get() : nullptr;
}

bool MySQLComponent::removeModel(int handle)
{
	return models_.erase(handle) > 0;
}

void MySQLComponent::clearModels()
{
	models_.clear();
}

void MySQLComponent::onFree(IComponent* component)
{
	// A component is being freed. Phase 2 holds no cross-component refs.
}

// Defined in natives.cpp — registers the classic AMX natives (those whose
// signature doesn't map cleanly onto SCRIPT_API, e.g. mysql_set_option's
// int-or-string value).
void MySQL_RegisterClassicNatives(AMX* amx);

void MySQLComponent::onAmxLoad(IPawnScript& script)
{
	// SCRIPT_API-declared natives auto-register here.
	pawn_natives::AmxLoad(script.GetAMX());
	// Classic int/string-value natives.
	MySQL_RegisterClassicNatives(script.GetAMX());
}

void MySQLComponent::onAmxUnload(IPawnScript& script)
{
}

// --- Async result dispatch (main thread) -----------------------------------

void MySQLComponent::applyModelResult(AMX* amx, const QueryJob& job)
{
	Model* m = getModel(job.modelHandle);
	if (m == nullptr)
	{
		return;
	}

	// INSERT: write the new auto-increment id back into the bound key variable.
	if (job.modelOp == MODEL_OP_INSERT)
	{
		const ModelField* kf = m->keyField();
		if (kf != nullptr && kf->addr != nullptr && job.succeeded && job.result
			&& !job.result->sets.empty())
		{
			*kf->addr = static_cast<ModelCell>(job.result->sets[0].insertId);
		}
		m->setError(job.succeeded ? ModelError::Ok : ModelError::Invalid);
		return;
	}

	if (job.modelOp != MODEL_OP_FIND)
	{
		// UPDATE / DELETE: only an error/ok status to record.
		m->setError(job.succeeded ? ModelError::Ok : ModelError::Invalid);
		return;
	}

	// FIND: populate bound variables from the single result row.
	if (!job.succeeded || !job.result || job.result->sets.empty()
		|| job.result->sets[0].rows.empty())
	{
		m->setError(ModelError::NoData);
		return;
	}
	const ResultSet& set = job.result->sets[0];
	const std::vector<ResultValue>& row = set.rows[0];
	for (const ModelField& f : m->fields())
	{
		if (f.addr == nullptr)
		{
			continue;
		}
		int col = set.fieldIndex(f.column);
		if (col < 0 || col >= static_cast<int>(row.size()))
		{
			continue;
		}
		const ResultValue& v = row[col];
		switch (f.type)
		{
		case ModelField::Type::Int:
			*f.addr = v.isNull ? 0
							   : static_cast<ModelCell>(std::strtol(v.value.c_str(), nullptr, 10));
			break;
		case ModelField::Type::Float:
		{
			float fv = v.isNull ? 0.0f : std::strtof(v.value.c_str(), nullptr);
			std::memcpy(f.addr, &fv, sizeof(fv));
			break;
		}
		case ModelField::Type::String:
			if (f.maxlen > 0)
			{
				amx_SetString(f.addr, v.isNull ? "" : v.value.c_str(), 0, 0,
					static_cast<size_t>(f.maxlen));
			}
			break;
		}
	}
	m->setError(ModelError::Ok);
}

void MySQLComponent::dispatchJob(const QueryJob& job)
{
	if (pawn_ == nullptr)
	{
		return;
	}

	// Resolve the AMX that owns this job's callback (it may be a filterscript, not
	// the gamemode). Fall back to the main script for the model write-back / hooks
	// when there's no named callback.
	AMX* amx = nullptr;
	if (!job.callback.empty())
	{
		amx = findScriptWithPublic(job.callback.c_str());
	}
	if (amx == nullptr && pawn_->mainScript() != nullptr)
	{
		amx = pawn_->mainScript()->GetAMX();
	}
	if (amx == nullptr)
	{
		return;
	}

	// Audit hook: fire OnQueryExecute for EVERY query that actually executed,
	// before any callback-skip logic, so a query the server ran is always logged
	// even if its (pquery) callback is later skipped. exec_time is the real
	// measurement taken in the worker. Failed queries go to OnQueryError instead.
	if (job.succeeded && job.result)
	{
		fireQueryExecuteHook(job.connectionHandle, job.sql,
			static_cast<long long>(job.result->execTimeMicros));
	}

	// Model (active-record) write-back: on a successful find, populate the bound
	// Pawn variables from the row; on insert, write the auto-increment id into
	// the bound key. Done before the callback so it sees the loaded values.
	if (job.modelHandle > 0)
	{
		applyModelResult(amx, job);
	}

	// A bound (pquery) callback is skipped if the player has since left.
	if (job.boundPlayer >= 0 && core_ != nullptr)
	{
		IPlayer* p = core_->getPlayers().get(job.boundPlayer);
		if (p == nullptr)
		{
			return;
		}
	}

	// On failure, the contract is OnQueryError(errorid, error, callback, query,
	// handle). We fire it whenever a query failed, regardless of callback.
	if (!job.succeeded)
	{
		AMX* errAmx = findScriptWithPublic("OnQueryError");
		int idx = -1;
		if (errAmx != nullptr && amx_FindPublic(errAmx, "OnQueryError", &idx) == AMX_ERR_NONE)
		{
			amx = errAmx;
			cell amxQuery = 0, amxError = 0, amxCallback = 0;
			cell* phys = nullptr;
			amx_PushString(amx, &amxQuery, &phys, job.sql.c_str(), 0, 0);
			amx_PushString(amx, &amxError, &phys, job.error.c_str(), 0, 0);
			amx_PushString(amx, &amxCallback, &phys, job.callback.c_str(), 0, 0);
			// forward OnQueryError(errorid, error[], callback[], query[], MySQL:handle)
			// push in REVERSE: handle, query, callback, error, errorid
			amx_Push(amx, static_cast<cell>(job.connectionHandle));
			amx_Push(amx, amxQuery);
			amx_Push(amx, amxCallback);
			amx_Push(amx, amxError);
			amx_Push(amx, static_cast<cell>(job.errorCode));
			cell ret = 0;
			amx_Exec(amx, &ret, idx);
			amx_Release(amx, amxCallback);
			amx_Release(amx, amxError);
			amx_Release(amx, amxQuery);
		}
		return;
	}

	if (job.callback.empty())
	{
		return; // fire-and-forget
	}

	int idx = -1;
	if (amx_FindPublic(amx, job.callback.c_str(), &idx) != AMX_ERR_NONE)
	{
		std::string msg = "callback \"" + job.callback + "\" not found";
		log(LogLevel::Warning, StringView(msg));
		return;
	}

	// Make this query's result the active cache for the callback to read via
	// the cache_* natives. A non-SELECT job may have an empty result.
	if (job.result)
	{
		setActiveCache(std::shared_ptr<QueryResult>(
			const_cast<QueryJob&>(job).result.release()), 0);
	}

	// Push the pre-marshalled args in REVERSE order. String args are released
	// after Exec.
	std::vector<cell> toRelease;
	for (auto it = job.args.rbegin(); it != job.args.rend(); ++it)
	{
		const CallbackArg& a = *it;
		switch (a.type)
		{
		case CallbackArg::Type::Int:
			amx_Push(amx, static_cast<cell>(a.intValue));
			break;
		case CallbackArg::Type::Float:
		{
			// float -> cell bit reinterpretation (portable; avoids amx_ftoc
			// which isn't always macro-visible here).
			float f = a.floatValue;
			cell c;
			static_assert(sizeof(c) == sizeof(f), "cell/float size mismatch");
			std::memcpy(&c, &f, sizeof(c));
			amx_Push(amx, c);
			break;
		}
		case CallbackArg::Type::String:
		{
			cell amxStr = 0;
			cell* phys = nullptr;
			amx_PushString(amx, &amxStr, &phys, a.stringValue.c_str(), 0, 0);
			toRelease.push_back(amxStr);
			break;
		}
		}
	}

	cell ret = 0;
	amx_Exec(amx, &ret, idx);

	for (auto rit = toRelease.rbegin(); rit != toRelease.rend(); ++rit)
	{
		amx_Release(amx, *rit);
	}

	// The active cache only lives for the duration of the callback.
	clearActiveCache();
}

void MySQLComponent::onTick(Microseconds elapsed, TimePoint now)
{
	(void)elapsed;
	(void)now;
	std::vector<std::unique_ptr<QueryJob>> done = engine_.drainCompleted();
	for (auto& job : done)
	{
		dispatchJob(*job);
	}
	std::vector<std::unique_ptr<HashJob>> hashes = engine_.drainHashes();
	for (auto& job : hashes)
	{
		dispatchHash(*job);
	}
}

void MySQLComponent::dispatchHash(const HashJob& job)
{
	if (pawn_ == nullptr || job.callback.empty())
	{
		return;
	}
	// The hash/verify callback may live in a filterscript (e.g. omp-admin's
	// OnRegisterHashed / OnLoginVerified), so search all scripts, not just main.
	AMX* amx = findScriptWithPublic(job.callback.c_str());
	if (amx == nullptr)
	{
		return;
	}
	int idx = -1;
	if (amx_FindPublic(amx, job.callback.c_str(), &idx) != AMX_ERR_NONE)
	{
		return;
	}

	// Callback shape:
	//   Hash:    forward OnHashed(<extra args...>, const hash[]);
	//   Verify:  forward OnVerified(<extra args...>, bool:success);
	// Push (reverse): the result first, then the pre-marshalled extra args.
	std::vector<cell> toRelease;
	cell resultStr = 0;
	if (job.op == HashJob::Op::Hash)
	{
		cell* phys = nullptr;
		amx_PushString(amx, &resultStr, &phys, job.result.c_str(), 0, 0);
		toRelease.push_back(resultStr);
	}
	else
	{
		amx_Push(amx, static_cast<cell>(job.verified ? 1 : 0));
	}

	for (auto it = job.args.rbegin(); it != job.args.rend(); ++it)
	{
		const CallbackArg& a = *it;
		switch (a.type)
		{
		case CallbackArg::Type::Int:
			amx_Push(amx, static_cast<cell>(a.intValue));
			break;
		case CallbackArg::Type::Float:
		{
			float f = a.floatValue;
			cell c;
			std::memcpy(&c, &f, sizeof c);
			amx_Push(amx, c);
			break;
		}
		case CallbackArg::Type::String:
		{
			cell s = 0;
			cell* phys = nullptr;
			amx_PushString(amx, &s, &phys, a.stringValue.c_str(), 0, 0);
			toRelease.push_back(s);
			break;
		}
		}
	}

	cell ret = 0;
	amx_Exec(amx, &ret, idx);
	for (auto rit = toRelease.rbegin(); rit != toRelease.rend(); ++rit)
	{
		amx_Release(amx, *rit);
	}
}

// --- Result cache ----------------------------------------------------------

const ResultSet* MySQLComponent::activeSet() const
{
	if (!activeCache_)
	{
		return nullptr;
	}
	if (activeResultIndex_ < 0 || activeResultIndex_ >= activeCache_->resultCount())
	{
		return nullptr;
	}
	return &activeCache_->sets[activeResultIndex_];
}

void MySQLComponent::setActiveCache(std::shared_ptr<QueryResult> result, int resultIndex)
{
	activeCache_ = std::move(result);
	activeResultIndex_ = resultIndex;
}

void MySQLComponent::clearActiveCache()
{
	activeCache_.reset();
	activeResultIndex_ = 0;
}

bool MySQLComponent::setActiveResultIndex(int index)
{
	if (!activeCache_ || index < 0 || index >= activeCache_->resultCount())
	{
		return false;
	}
	activeResultIndex_ = index;
	return true;
}

int MySQLComponent::saveActiveCache()
{
	if (!activeCache_)
	{
		return 0;
	}
	int id = nextCache_++;
	savedCaches_.emplace(id, activeCache_);
	return id;
}

bool MySQLComponent::activateSavedCache(int cacheId)
{
	auto it = savedCaches_.find(cacheId);
	if (it == savedCaches_.end())
	{
		return false;
	}
	activeCache_ = it->second;
	activeResultIndex_ = 0;
	return true;
}

bool MySQLComponent::deleteSavedCache(int cacheId)
{
	return savedCaches_.erase(cacheId) > 0;
}

bool MySQLComponent::isValidCache(int cacheId) const
{
	return savedCaches_.find(cacheId) != savedCaches_.end();
}

void MySQLComponent::free()
{
	clearActiveCache();
	savedCaches_.clear();
	// Statements own a MYSQL_STMT* on a connection — free them before the
	// connections (and their worker threads) go away.
	clearStatements();
	clearModels();
	clearConnections();
	clearOptions();
	delete this;
}

void MySQLComponent::reset()
{
	// On GMX (gamemode restart) drop all script-owned connections + options +
	// cached results + statements.
	clearActiveCache();
	savedCaches_.clear();
	nextCache_ = 1;
	clearStatements();
	nextStatement_ = 1;
	clearModels();
	nextModel_ = 1;
	clearConnections();
	clearOptions();
	nextHandle_ = 1;
	nextOptions_ = 1;
}

MySQLComponent::~MySQLComponent()
{
	clearConnections();
	if (pawn_ != nullptr)
	{
		pawn_->getEventDispatcher().removeEventHandler(this);
	}
	if (core_ != nullptr)
	{
		core_->getEventDispatcher().removeEventHandler(this);
	}
	instance_ = nullptr;
}
