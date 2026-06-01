/* =========================================
 *
 *  MySQL for open.mp  —  password hashing impl
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#include "Hashing.hpp"

#include <argon2.h>

#include <random>
#include <vector>

namespace Hashing
{
// Fill `out` with cryptographically-strong random bytes.
static void csprng(unsigned char* out, size_t n)
{
	std::random_device rd; // OS entropy source
	for (size_t i = 0; i < n; ++i)
	{
		out[i] = static_cast<unsigned char>(rd() & 0xFF);
	}
}

std::string hashArgon2id(const std::string& password, const Argon2Params& p)
{
	std::vector<unsigned char> salt(p.saltLen);
	csprng(salt.data(), salt.size());

	size_t encodedLen = argon2_encodedlen(
		p.timeCost, p.memoryKiB, p.parallelism, p.saltLen, p.hashLen, Argon2_id);
	std::vector<char> encoded(encodedLen + 1, '\0');

	int rc = argon2id_hash_encoded(
		p.timeCost, p.memoryKiB, p.parallelism,
		password.data(), password.size(),
		salt.data(), salt.size(),
		p.hashLen, encoded.data(), encoded.size());

	if (rc != ARGON2_OK)
	{
		return {};
	}
	return std::string(encoded.data());
}

bool verify(const std::string& password, const std::string& encoded)
{
	if (encoded.empty())
	{
		return false;
	}
	// argon2_verify is constant-time and auto-detects the variant from the
	// encoded string's prefix ($argon2id$ / $argon2i$ / $argon2d$).
	int rc = argon2_verify(encoded.c_str(), password.data(), password.size(), Argon2_id);
	if (rc == ARGON2_OK)
	{
		return true;
	}
	// Fall back to the other variants for hashes produced elsewhere.
	if (argon2_verify(encoded.c_str(), password.data(), password.size(), Argon2_i) == ARGON2_OK)
	{
		return true;
	}
	if (argon2_verify(encoded.c_str(), password.data(), password.size(), Argon2_d) == ARGON2_OK)
	{
		return true;
	}
	return false;
}
}
