/* =========================================
 *
 *  MySQL for open.mp  —  buffered result set
 *  ------------------------------------------
 *
 *  A query's results are fully copied into memory ON THE WORKER THREAD (we can't
 *  keep a live MYSQL_RES* around for the main thread to read lazily — the
 *  connection moves on to the next query). The main thread then serves the
 *  cache_* natives entirely from these buffers.
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// One column's metadata.
struct ResultField
{
	std::string name;
	int type = -1; // enum_field_types value (matches E_MYSQL_FIELD_TYPE)
};

// One cell value. NULL is tracked explicitly (empty string is a valid non-null
// value and must be distinguishable from SQL NULL).
struct ResultValue
{
	std::string value;
	bool isNull = false;
};

// One result set (one statement's worth of rows). A multi-statement query
// yields several of these.
struct ResultSet
{
	std::vector<ResultField> fields;
	std::vector<std::vector<ResultValue>> rows; // rows[r][c]

	uint64_t affectedRows = 0;
	uint64_t insertId = 0;
	unsigned int warningCount = 0;

	int rowCount() const { return static_cast<int>(rows.size()); }
	int fieldCount() const { return static_cast<int>(fields.size()); }

	// -1 if no such column name.
	int fieldIndex(const std::string& name) const
	{
		for (size_t i = 0; i < fields.size(); ++i)
		{
			if (fields[i].name == name)
			{
				return static_cast<int>(i);
			}
		}
		return -1;
	}
};

// All result sets a single query produced, plus how long it took. Owned first by
// the QueryJob, then (if the script keeps it) by the component's saved-cache map.
struct QueryResult
{
	std::vector<ResultSet> sets;
	int64_t execTimeMicros = 0;
	std::string queryString;

	int resultCount() const { return static_cast<int>(sets.size()); }
};
