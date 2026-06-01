/* =========================================
 *
 *  MySQL for open.mp  —  prepared statement impl
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#include "Statement.hpp"
#include "Connection.hpp"

#include <mysql.h>

#include <cstring>
#include <memory>
#include <type_traits>

#define STMT() (static_cast<MYSQL_STMT*>(stmt_))

// Count `?` parameter placeholders in `sql`, ignoring any that appear inside a
// string/identifier literal or a comment. This lets us size the parameter staging
// vector on the MAIN thread (so binds validate by index) without preparing the
// statement on the server yet — prepare is deferred to the worker in execute().
static int CountPlaceholders(const std::string& sql)
{
	int count = 0;
	char quote = 0;       // 0, or the open quote char ' " `
	bool lineComment = false;
	bool blockComment = false;
	for (size_t i = 0; i < sql.size(); ++i)
	{
		char c = sql[i];
		char n = (i + 1 < sql.size()) ? sql[i + 1] : '\0';
		if (lineComment)
		{
			if (c == '\n') lineComment = false;
			continue;
		}
		if (blockComment)
		{
			if (c == '*' && n == '/') { blockComment = false; ++i; }
			continue;
		}
		if (quote != 0)
		{
			// Backslash-escape (skip next) then matched closing quote.
			if (c == '\\' && quote != '`') { ++i; continue; }
			if (c == quote) quote = 0;
			continue;
		}
		if (c == '-' && n == '-') { lineComment = true; ++i; continue; }
		if (c == '#') { lineComment = true; continue; }
		if (c == '/' && n == '*') { blockComment = true; ++i; continue; }
		if (c == '\'' || c == '"' || c == '`') { quote = c; continue; }
		if (c == '?') ++count;
	}
	return count;
}

PreparedStatement::PreparedStatement(Connection* conn, const std::string& query)
	: conn_(conn)
	, query_(query)
{
	// NO libmysql calls here: this runs on the main thread, and conn_->raw() is
	// owned by the worker thread. We only size the parameter staging vector by
	// counting placeholders; the real prepare happens lazily in execute().
	if (conn_ == nullptr || query_.empty())
	{
		return;
	}
	paramCount_ = CountPlaceholders(query_);
	params_.resize(static_cast<size_t>(paramCount_));
}

PreparedStatement::~PreparedStatement()
{
	if (stmt_ != nullptr)
	{
		mysql_stmt_close(STMT());
		stmt_ = nullptr;
	}
}

// Ensure params_[index] exists. Binds happen on the main thread before the worker
// has prepared the statement, so paramCount_ is only a placeholder-count estimate;
// grow to fit a valid index and let execute() reconcile against the server count.
bool PreparedStatement::ensureParam(int index)
{
	if (index < 0)
	{
		return false;
	}
	if (static_cast<size_t>(index) >= params_.size())
	{
		params_.resize(static_cast<size_t>(index) + 1);
		if (index + 1 > paramCount_)
		{
			paramCount_ = index + 1;
		}
	}
	return true;
}

void PreparedStatement::bindInt(int index, int value)
{
	if (!ensureParam(index))
	{
		return;
	}
	params_[index].kind = StmtParam::Kind::Int;
	params_[index].intValue = value;
}

void PreparedStatement::bindFloat(int index, float value)
{
	if (!ensureParam(index))
	{
		return;
	}
	params_[index].kind = StmtParam::Kind::Float;
	params_[index].floatValue = value;
}

void PreparedStatement::bindString(int index, const std::string& value)
{
	if (!ensureParam(index))
	{
		return;
	}
	params_[index].kind = StmtParam::Kind::String;
	params_[index].stringValue = value;
}

void PreparedStatement::bindNull(int index)
{
	if (!ensureParam(index))
	{
		return;
	}
	params_[index].kind = StmtParam::Kind::Null;
}

bool PreparedStatement::execute(QueryResult& out)
{
	out.sets.clear();
	if (!valid())
	{
		return false;
	}

	// Lazy prepare on the WORKER thread (this method only ever runs there). Doing
	// mysql_stmt_init / mysql_stmt_prepare here — not in the ctor — keeps every
	// libmysql call on the one thread that owns conn_->raw(). Preparing on the main
	// thread raced the worker and corrupted the statement on Windows/wine.
	if (!prepared_)
	{
		if (conn_->raw() == nullptr)
		{
			return false;
		}
		MYSQL* m = static_cast<MYSQL*>(conn_->raw());
		stmt_ = mysql_stmt_init(m);
		if (stmt_ == nullptr)
		{
			return false;
		}
		if (mysql_stmt_prepare(STMT(), query_.c_str(),
				static_cast<unsigned long>(query_.size())) != 0)
		{
			return false;
		}
		// Trust the server's authoritative parameter count; reconcile our
		// placeholder estimate so binds line up even if the parser miscounted.
		int serverParams = static_cast<int>(mysql_stmt_param_count(STMT()));
		if (serverParams != paramCount_)
		{
			paramCount_ = serverParams;
			params_.resize(static_cast<size_t>(paramCount_));
		}
		prepared_ = true;
	}

	MYSQL_STMT* st = STMT();

	// The connector's flag type for is_null/error differs across versions:
	// `bool` on Connector/C 8.x/9.x, `my_bool` (== char) on 6.1.x / the Windows
	// build. Derive it from MYSQL_BIND so the same code is correct everywhere.
	using BindFlag = std::remove_pointer_t<decltype(MYSQL_BIND::is_null)>;

	// --- bind input parameters ---
	std::vector<MYSQL_BIND> binds(static_cast<size_t>(paramCount_));
	std::memset(binds.data(), 0, binds.size() * sizeof(MYSQL_BIND));
	// Backing storage that must outlive mysql_stmt_execute.
	std::vector<long long> ints(static_cast<size_t>(paramCount_), 0);
	std::vector<double> dbls(static_cast<size_t>(paramCount_), 0.0);
	std::vector<unsigned long> lens(static_cast<size_t>(paramCount_), 0);
	// is_null wants a BindFlag*; std::vector<bool> is bit-packed and can't yield
	// a pointer, so use a heap array of the exact flag type.
	auto nulls = std::make_unique<BindFlag[]>(static_cast<size_t>(paramCount_));

	for (int i = 0; i < paramCount_; ++i)
	{
		MYSQL_BIND& b = binds[i];
		StmtParam& p = params_[i];
		switch (p.kind)
		{
		case StmtParam::Kind::Int:
			ints[i] = p.intValue;
			b.buffer_type = MYSQL_TYPE_LONGLONG;
			b.buffer = &ints[i];
			b.is_unsigned = 0;
			break;
		case StmtParam::Kind::Float:
			dbls[i] = p.floatValue;
			b.buffer_type = MYSQL_TYPE_DOUBLE;
			b.buffer = &dbls[i];
			break;
		case StmtParam::Kind::String:
			lens[i] = static_cast<unsigned long>(p.stringValue.size());
			b.buffer_type = MYSQL_TYPE_STRING;
			b.buffer = const_cast<char*>(p.stringValue.data());
			b.buffer_length = lens[i];
			b.length = &lens[i];
			break;
		case StmtParam::Kind::Null:
		default:
			nulls[i] = true;
			b.buffer_type = MYSQL_TYPE_NULL;
			b.is_null = &nulls[i];
			break;
		}
	}

	if (paramCount_ > 0 && mysql_stmt_bind_param(st, binds.data()) != 0)
	{
		return false;
	}

	if (mysql_stmt_execute(st) != 0)
	{
		return false;
	}

	ResultSet set;
	set.affectedRows = mysql_stmt_affected_rows(st);
	set.insertId = mysql_stmt_insert_id(st);

	// --- result set (if the statement produced one) ---
	MYSQL_RES* meta = mysql_stmt_result_metadata(st);
	if (meta == nullptr)
	{
		// Non-SELECT statement: just metadata.
		out.sets.push_back(std::move(set));
		return true;
	}

	unsigned int nfields = mysql_num_fields(meta);
	MYSQL_FIELD* fields = mysql_fetch_fields(meta);
	set.fields.reserve(nfields);
	for (unsigned int i = 0; i < nfields; ++i)
	{
		ResultField f;
		f.name = fields[i].name ? fields[i].name : "";
		f.type = static_cast<int>(fields[i].type);
		set.fields.push_back(std::move(f));
	}

	// Bind output buffers. We fetch every column as text (a generous per-cell
	// buffer, grown via mysql_stmt_fetch_column when truncated).
	const unsigned long kCellBuf = 1024;
	std::vector<std::vector<char>> cellBufs(nfields, std::vector<char>(kCellBuf));
	std::vector<unsigned long> outLens(nfields, 0);
	auto outNulls = std::make_unique<BindFlag[]>(nfields);
	auto outErrors = std::make_unique<BindFlag[]>(nfields);
	std::vector<MYSQL_BIND> outBinds(nfields);
	std::memset(outBinds.data(), 0, outBinds.size() * sizeof(MYSQL_BIND));
	for (unsigned int c = 0; c < nfields; ++c)
	{
		outBinds[c].buffer_type = MYSQL_TYPE_STRING;
		outBinds[c].buffer = cellBufs[c].data();
		outBinds[c].buffer_length = kCellBuf;
		outBinds[c].length = &outLens[c];
		outBinds[c].is_null = &outNulls[c];
		outBinds[c].error = &outErrors[c];
	}

	if (mysql_stmt_bind_result(st, outBinds.data()) != 0
		|| mysql_stmt_store_result(st) != 0)
	{
		mysql_free_result(meta);
		return false;
	}

	int fetchStatus;
	while ((fetchStatus = mysql_stmt_fetch(st)) == 0 || fetchStatus == MYSQL_DATA_TRUNCATED)
	{
		std::vector<ResultValue> cells;
		cells.reserve(nfields);
		for (unsigned int c = 0; c < nfields; ++c)
		{
			ResultValue v;
			if (outNulls[c])
			{
				v.isNull = true;
			}
			else if (outLens[c] <= kCellBuf)
			{
				v.value.assign(cellBufs[c].data(), outLens[c]);
			}
			else
			{
				// Value was truncated; refetch this column with a big-enough buffer.
				std::vector<char> big(outLens[c] + 1, '\0');
				MYSQL_BIND rb;
				std::memset(&rb, 0, sizeof(rb));
				unsigned long rlen = 0;
				rb.buffer_type = MYSQL_TYPE_STRING;
				rb.buffer = big.data();
				rb.buffer_length = static_cast<unsigned long>(big.size());
				rb.length = &rlen;
				if (mysql_stmt_fetch_column(st, &rb, c, 0) == 0)
				{
					v.value.assign(big.data(), rlen);
				}
			}
			cells.push_back(std::move(v));
		}
		set.rows.push_back(std::move(cells));
	}

	mysql_free_result(meta);
	out.sets.push_back(std::move(set));
	return true;
}

unsigned int PreparedStatement::lastErrno() const
{
	return stmt_ != nullptr ? mysql_stmt_errno(STMT()) : 0;
}

std::string PreparedStatement::lastError() const
{
	if (stmt_ == nullptr)
	{
		return "invalid statement";
	}
	const char* e = mysql_stmt_error(STMT());
	return e != nullptr ? std::string(e) : std::string();
}
