/* =========================================
 *
 *  omp-mySQL  —  active-record Model impl (SQL generation)
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#include "Model.hpp"

namespace
{
// Backtick-quote an identifier, doubling any embedded backticks.
std::string ident(const std::string& s)
{
	std::string out = "`";
	for (char c : s)
	{
		if (c == '`')
		{
			out += "``";
		}
		else
		{
			out += c;
		}
	}
	out += '`';
	return out;
}
} // namespace

void Model::upsertField(const std::string& column, ModelField::Type t,
	ModelCell* addr, int maxlen)
{
	for (ModelField& f : fields_)
	{
		if (f.column == column)
		{
			f.type = t;
			f.addr = addr;
			f.maxlen = maxlen;
			return;
		}
	}
	fields_.push_back(ModelField { column, t, addr, maxlen });
}

void Model::bindInt(const std::string& column, ModelCell* addr)
{
	upsertField(column, ModelField::Type::Int, addr, 0);
}

void Model::bindFloat(const std::string& column, ModelCell* addr)
{
	upsertField(column, ModelField::Type::Float, addr, 0);
}

void Model::bindString(const std::string& column, ModelCell* addr, int maxlen)
{
	upsertField(column, ModelField::Type::String, addr, maxlen);
}

bool Model::unbind(const std::string& column)
{
	for (auto it = fields_.begin(); it != fields_.end(); ++it)
	{
		if (it->column == column)
		{
			fields_.erase(it);
			return true;
		}
	}
	return false;
}

const ModelField* Model::keyField() const
{
	if (key_.empty())
	{
		return nullptr;
	}
	for (const ModelField& f : fields_)
	{
		if (f.column == key_)
		{
			return &f;
		}
	}
	return nullptr;
}

// SELECT <cols> FROM <table> WHERE <key> = <keyValue> LIMIT 1
std::string Model::buildFind(const std::string& keyValue) const
{
	if (key_.empty() || fields_.empty())
	{
		return {};
	}
	std::string sql = "SELECT ";
	for (size_t i = 0; i < fields_.size(); ++i)
	{
		if (i)
		{
			sql += ", ";
		}
		sql += ident(fields_[i].column);
	}
	sql += " FROM " + ident(table_) + " WHERE " + ident(key_) + " = " + keyValue
		+ " LIMIT 1";
	return sql;
}

// INSERT INTO <table> (<cols>) VALUES (<values>)
std::string Model::buildInsert(const std::vector<std::string>& values) const
{
	if (fields_.empty() || values.size() != fields_.size())
	{
		return {};
	}
	std::string cols, vals;
	for (size_t i = 0; i < fields_.size(); ++i)
	{
		if (i)
		{
			cols += ", ";
			vals += ", ";
		}
		cols += ident(fields_[i].column);
		vals += values[i];
	}
	return "INSERT INTO " + ident(table_) + " (" + cols + ") VALUES (" + vals + ")";
}

// UPDATE <table> SET <col=val, ...> WHERE <key> = <keyValue>
// The key column is not written in the SET list.
std::string Model::buildUpdate(const std::vector<std::string>& values,
	const std::string& keyValue) const
{
	if (key_.empty() || fields_.empty() || values.size() != fields_.size())
	{
		return {};
	}
	std::string sets;
	bool first = true;
	for (size_t i = 0; i < fields_.size(); ++i)
	{
		if (fields_[i].column == key_)
		{
			continue; // never UPDATE the key in the SET clause
		}
		if (!first)
		{
			sets += ", ";
		}
		first = false;
		sets += ident(fields_[i].column) + " = " + values[i];
	}
	if (sets.empty())
	{
		return {}; // nothing to update besides the key
	}
	return "UPDATE " + ident(table_) + " SET " + sets + " WHERE " + ident(key_)
		+ " = " + keyValue;
}

// DELETE FROM <table> WHERE <key> = <keyValue>
std::string Model::buildDelete(const std::string& keyValue) const
{
	if (key_.empty())
	{
		return {};
	}
	return "DELETE FROM " + ident(table_) + " WHERE " + ident(key_) + " = "
		+ keyValue;
}
