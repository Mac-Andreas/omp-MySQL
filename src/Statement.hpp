/* =========================================
 *
 *  MySQL for open.mp  —  prepared statement
 *  ------------------------------------------
 *
 *  Wraps a MYSQL_STMT*. Parameters are staged as typed values on the main
 *  thread, then bound + executed on the connection's worker thread (the only
 *  thread that touches this connection's MYSQL*). Results are buffered into the
 *  shared QueryResult so the cache_* natives read them exactly like a normal
 *  query.
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#pragma once

#include "Result.hpp"

#include <string>
#include <vector>
#include <cstdint>

class Connection;

// One staged parameter value. NULL and the three scalar kinds; everything is
// sent to the server in its native MYSQL_BIND buffer type.
struct StmtParam
{
	enum class Kind
	{
		Null,
		Int,
		Float,
		String,
	};

	Kind kind = Kind::Null;
	int64_t intValue = 0;
	double floatValue = 0.0;
	std::string stringValue;
};

class PreparedStatement
{
public:
	// Stages `query` against `conn`. The actual mysql_stmt_init + mysql_stmt_prepare
	// is DEFERRED to the first execute() (which runs on the connection's worker
	// thread) — preparing here on the main thread would race the worker that owns
	// this connection's MYSQL*. After construction, check valid().
	PreparedStatement(Connection* conn, const std::string& query);
	~PreparedStatement();

	PreparedStatement(const PreparedStatement&) = delete;
	PreparedStatement& operator=(const PreparedStatement&) = delete;

	// valid() == "usable": a non-empty query on a live connection. The statement is
	// not actually prepared on the server until the first execute() (on the worker).
	bool valid() const { return conn_ != nullptr && !query_.empty(); }
	int paramCount() const { return paramCount_; }
	Connection* connection() const { return conn_; }
	// The prepared SQL with `?` placeholders intact (bound values never inlined).
	const std::string& queryText() const { return query_; }

	// Stage a parameter value at `index` (0-based). Out-of-range is ignored.
	void bindInt(int index, int value);
	void bindFloat(int index, float value);
	void bindString(int index, const std::string& value);
	void bindNull(int index);

	// Bind the staged params and execute, buffering any result set into `out`.
	// Runs on the worker thread. Returns true on success.
	bool execute(QueryResult& out);

	unsigned int lastErrno() const;
	std::string lastError() const;

private:
	// Grow params_ to include `index` (binds run before the worker prepares, so
	// paramCount_ starts as a placeholder estimate). Returns false if index < 0.
	bool ensureParam(int index);

	Connection* conn_ = nullptr;
	void* stmt_ = nullptr; // MYSQL_STMT*
	bool prepared_ = false;
	int paramCount_ = 0;
	std::vector<StmtParam> params_;
	std::string query_; // the prepared SQL (placeholders intact), for auditing
};
