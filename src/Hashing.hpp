/* =========================================
 *
 *  MySQL for open.mp  —  password hashing
 *  ------------------------------------------
 *
 *  Argon2id password hashing (OWASP-recommended, memory-hard). Hashing is
 *  deliberately slow, so it runs on a worker thread (see HashJob). The encoded
 *  output embeds the algorithm, parameters and a CSPRNG salt, so verification
 *  needs only the password and the stored hash. Verification is constant-time
 *  (argon2_verify).
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#pragma once

#include <string>

namespace Hashing
{
// OWASP-tuned defaults (interactive login): 3 iterations, 64 MiB, 1 lane.
struct Argon2Params
{
	unsigned int timeCost = 3;
	unsigned int memoryKiB = 64 * 1024;
	unsigned int parallelism = 1;
	unsigned int saltLen = 16;
	unsigned int hashLen = 32;
};

/// Hash `password` with Argon2id. Returns the encoded string (empty on failure).
std::string hashArgon2id(const std::string& password, const Argon2Params& p = {});

/// Constant-time verify of `password` against an encoded Argon2 hash.
bool verify(const std::string& password, const std::string& encoded);
}
