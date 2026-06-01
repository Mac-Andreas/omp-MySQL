/* =========================================
 *
 *  MySQL for open.mp  —  async hashing job
 *  ------------------------------------------
 *
 *  Password hashing/verification is CPU-heavy and deliberately slow, so it runs
 *  on a worker thread and the result is delivered on the main tick (like a
 *  query callback). Not tied to any connection.
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#pragma once

#include "Query.hpp" // CallbackArg

#include <string>
#include <vector>

struct HashJob
{
	enum class Op
	{
		Hash,
		Verify,
	};

	Op op = Op::Hash;
	std::string password;
	std::string hash; // input hash (verify) — unused for Hash

	std::string callback;          // Pawn public to call
	std::vector<CallbackArg> args; // pre-marshalled extra callback args

	// --- filled in by the worker ---
	std::string result; // encoded hash (Hash op)
	bool verified = false; // result of Verify
};
