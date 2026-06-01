/* =========================================
 *
 *  omp-mySQL  —  active-record Model (the mysql_model_* layer)
 *  ------------------------------------------
 *
 *  Maps one table row to a set of Pawn variables. The script binds globals to
 *  columns; the model holds the binding metadata and generates SQL for
 *  find/insert/update/delete. Reading the bound Pawn variables and writing
 *  results back into them is done in natives.cpp (it needs AMX access); this
 *  class is AMX-free so it can be unit-reasoned and stays out of the pawn-impl
 *  translation unit.
 *
 *  Bound variables are referenced by their AMX physical cell address (captured
 *  at bind time) — so they MUST be globals that outlive the async call.
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

using ModelCell = std::int32_t; // AMX cell

// One column <-> Pawn variable binding.
struct ModelField
{
	enum class Type { Int, Float, String };

	std::string column;
	Type type = Type::Int;
	ModelCell* addr = nullptr; // AMX physical address of the bound variable
	int maxlen = 0;            // string capacity in cells (incl. terminator)
};

// Mirrors E_MYSQL_MODEL_ERROR in omp-mysql.inc.
enum class ModelError : int { Ok = 0, NoData = 1, Invalid = 2 };

// QueryJob::modelOp values (kept as plain ints on the job to avoid the include).
enum ModelOp { MODEL_OP_FIND = 1, MODEL_OP_INSERT = 2, MODEL_OP_UPDATE = 3,
	MODEL_OP_DELETE = 4 };

class Model
{
public:
	Model(int connectionHandle, std::string table)
		: connectionHandle_(connectionHandle)
		, table_(std::move(table))
	{
	}

	int connectionHandle() const { return connectionHandle_; }
	ModelError lastError() const { return lastError_; }
	void setError(ModelError e) { lastError_ = e; }

	void bindInt(const std::string& column, ModelCell* addr);
	void bindFloat(const std::string& column, ModelCell* addr);
	void bindString(const std::string& column, ModelCell* addr, int maxlen);
	bool unbind(const std::string& column);
	void clear() { fields_.clear(); }
	void setKey(const std::string& column) { key_ = column; }
	const std::string& key() const { return key_; }

	const std::vector<ModelField>& fields() const { return fields_; }
	const ModelField* keyField() const;
	const std::string& table() const { return table_; }

	// Build SQL for an operation. `values` are the bound fields' current values,
	// already escaped+quoted (string) or rendered (number) by the caller — same
	// order as fields(). `keyValue` is the bound key's current literal. Returns
	// "" if the op can't be built (e.g. find/update/delete with no key).
	std::string buildFind(const std::string& keyValue) const;
	std::string buildInsert(const std::vector<std::string>& values) const;
	std::string buildUpdate(const std::vector<std::string>& values,
		const std::string& keyValue) const;
	std::string buildDelete(const std::string& keyValue) const;

private:
	void upsertField(const std::string& column, ModelField::Type t,
		ModelCell* addr, int maxlen);

	int connectionHandle_;
	std::string table_;
	std::string key_;
	std::vector<ModelField> fields_;
	ModelError lastError_ = ModelError::Ok;
};
