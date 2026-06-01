/* =========================================
 *
 *  MySQL for open.mp  —  async query job
 *  ------------------------------------------
 *
 *  A unit of asynchronous work: the SQL to run, the Pawn callback to fire when
 *  it finishes, and the marshalled callback arguments. Created on the main
 *  thread, executed on a worker, then handed back to the main thread to invoke
 *  the callback. Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#pragma once

#include "Result.hpp"

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// One pre-marshalled callback argument. The `format` mini-language a script
// passes to mysql_tquery is decoded on the MAIN thread (we can't touch the AMX
// from a worker), producing these typed values; the worker never sees the AMX.
struct CallbackArg
{
	enum class Type
	{
		Int,
		Float,
		String,
	};

	Type type = Type::Int;
	int intValue = 0;
	float floatValue = 0.0f;
	std::string stringValue;

	static CallbackArg makeInt(int v)
	{
		CallbackArg a;
		a.type = Type::Int;
		a.intValue = v;
		return a;
	}
	static CallbackArg makeFloat(float v)
	{
		CallbackArg a;
		a.type = Type::Float;
		a.floatValue = v;
		return a;
	}
	static CallbackArg makeString(std::string v)
	{
		CallbackArg a;
		a.type = Type::String;
		a.stringValue = std::move(v);
		return a;
	}
};

class PreparedStatement;

// A query job flowing main -> worker -> main.
struct QueryJob
{
	int connectionHandle = 0;
	std::string sql;

	// For a prepared-statement execute job: the statement to bind+run instead of
	// `sql`. Not owned (the script owns it via its handle); the worker only
	// borrows it for the duration of execute(). null for a plain query job.
	PreparedStatement* statement = nullptr;

	// Statement teardown job: the worker destroys this owned statement (running
	// mysql_stmt_close on the thread that owns the connection's MYSQL*, never the
	// main thread). When set, this job does nothing but drop the unique_ptr. No
	// callback is fired. See MySQLComponent::removeStatement.
	std::unique_ptr<PreparedStatement> statementToClose;

	std::string callback;            // Pawn public to call ("" = fire-and-forget)
	std::vector<CallbackArg> args;   // pre-marshalled callback arguments

	// If bound to a player (mysql_pquery), the callback is skipped when this
	// player is no longer connected by the time results come back. -1 = none.
	int boundPlayer = -1;

	// Whether to keep the result accessible after the callback (mysql_query's
	// use_cache, or any tquery/pquery — the active cache lives for the callback).
	bool useCache = true;

	// --- model (active-record) jobs ------------------------------------------
	// When >0, this job came from a mysql_model_* op; on the main thread,
	// dispatchJob writes the result back into the model's bound Pawn variables
	// (find) or the insert-id into the key (insert) BEFORE firing the callback.
	int modelHandle = 0;
	int modelOp = 0; // ModelOp, but kept int to avoid the include here

	// --- filled in by the worker ---
	bool succeeded = false;
	unsigned int errorCode = 0;
	std::string error;

	// Fully-buffered results (rows copied off the worker thread). May be empty
	// for non-SELECT statements (only affected-rows/insert-id metadata then).
	std::unique_ptr<QueryResult> result;
};
