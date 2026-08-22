#include "retdec/neural/model_verify.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include <rapidjson/document.h>

#if defined(RETDEC_HAS_LLAMACPP)
#include "gguf.h"
#endif

#if defined(RETDEC_NEURAL_HAS_OPENSSL)
#include <openssl/evp.h>
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x10100000L
#include <openssl/opensslv.h>
#endif
#endif

namespace retdec::neural {

namespace {

// SHA-256 of the Unsloth llama.cpp-native Q4_K_M (5.28 GiB).
// Ollama qwen3.5:9b blobs fail on b10451 (rope.dimension_sections length 3).
constexpr const char* kQwen35Q4KmSha256 = "03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8";

std::string fileBasename(const std::string& path)
{
	const auto slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool namesMatchHint(const std::string& path)
{
	auto name = fileBasename(path);
	const char* hints[] = {
		kQwen35TextOnlyGgufHint,
		"Qwen3.5-9B-Q4_K_M.gguf",
		"Qwen3.5-9B-Q4_K_M.unsloth.gguf",
	};
	for (const char* hint: hints)
	{
		const std::string h = hint;
		if (name.size() != h.size()) continue;
		bool ok = true;
		for (std::size_t i = 0; i < name.size(); ++i)
		{
			const auto a = static_cast<unsigned char>(name[i]);
			const auto b = static_cast<unsigned char>(h[i]);
			if (std::tolower(a) != std::tolower(b))
			{
				ok = false;
				break;
			}
		}
		if (ok) return true;
	}
	return false;
}

std::string digestToHex(const unsigned char* digest, unsigned int len)
{
	static const char* kHex = "0123456789abcdef";
	std::string hex;
	hex.resize(static_cast<std::size_t>(len) * 2);
	for (unsigned int i = 0; i < len; ++i)
	{
		hex[i * 2] = kHex[digest[i] >> 4];
		hex[i * 2 + 1] = kHex[digest[i] & 0x0f];
	}
	return hex;
}

#if defined(RETDEC_NEURAL_HAS_OPENSSL)

std::string sha256HexOfFile(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) return {};

#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x10100000L
	EVP_MD_CTX ctxStorage;
	EVP_MD_CTX* ctx = &ctxStorage;
	EVP_MD_CTX_init(ctx);
#else
	EVP_MD_CTX* ctx = EVP_MD_CTX_new();
	if (!ctx) return {};
#endif

	if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1)
	{
#if !defined(OPENSSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER >= 0x10100000L
		EVP_MD_CTX_free(ctx);
#else
		EVP_MD_CTX_cleanup(ctx);
#endif
		return {};
	}

	char buf[8192];
	while (in)
	{
		in.read(buf, sizeof(buf));
		const auto n = in.gcount();
		if (n <= 0) break;
		if (EVP_DigestUpdate(ctx, buf, static_cast<std::size_t>(n)) != 1)
		{
#if !defined(OPENSSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER >= 0x10100000L
			EVP_MD_CTX_free(ctx);
#else
			EVP_MD_CTX_cleanup(ctx);
#endif
			return {};
		}
	}

	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int len = 0;
	const int ok = EVP_DigestFinal_ex(ctx, digest, &len);
#if !defined(OPENSSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER >= 0x10100000L
	EVP_MD_CTX_free(ctx);
#else
	EVP_MD_CTX_cleanup(ctx);
#endif
	if (ok != 1 || len == 0) return {};
	return digestToHex(digest, len);
}

#else

// FIPS 180-4 SHA-256 over streamed file chunks (no popen / no std::system).
class Sha256Stream
{
public:
	Sha256Stream() { reset(); }

	void reset()
	{
		h_[0] = 0x6a09e667u;
		h_[1] = 0xbb67ae85u;
		h_[2] = 0x3c6ef372u;
		h_[3] = 0xa54ff53au;
		h_[4] = 0x510e527fu;
		h_[5] = 0x9b05688cu;
		h_[6] = 0x1f83d9abu;
		h_[7] = 0x5be0cd19u;
		nbits_ = 0;
		bufLen_ = 0;
	}

	void update(const void* data, std::size_t len)
	{
		const auto* p = static_cast<const std::uint8_t*>(data);
		nbits_ += static_cast<std::uint64_t>(len) * 8u;
		while (len > 0)
		{
			const std::size_t take = len < (64 - bufLen_) ? len : (64 - bufLen_);
			std::memcpy(buf_ + bufLen_, p, take);
			bufLen_ += take;
			p += take;
			len -= take;
			if (bufLen_ == 64)
			{
				compress(buf_);
				bufLen_ = 0;
			}
		}
	}

	void final(std::uint8_t out[32])
	{
		buf_[bufLen_++] = 0x80;
		if (bufLen_ > 56)
		{
			while (bufLen_ < 64) buf_[bufLen_++] = 0;
			compress(buf_);
			bufLen_ = 0;
		}
		while (bufLen_ < 56) buf_[bufLen_++] = 0;
		for (int i = 7; i >= 0; --i)
			buf_[bufLen_++] = static_cast<std::uint8_t>(nbits_ >> (i * 8));
		compress(buf_);
		for (int i = 0; i < 8; ++i)
		{
			out[i * 4 + 0] = static_cast<std::uint8_t>(h_[i] >> 24);
			out[i * 4 + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
			out[i * 4 + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
			out[i * 4 + 3] = static_cast<std::uint8_t>(h_[i]);
		}
	}

private:
	static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

	void compress(const std::uint8_t* p)
	{
		static const std::uint32_t K[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
		std::uint32_t w[64];
		for (int i = 0; i < 16; ++i)
		{
			w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) | (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16)
				 | (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(p[i * 4 + 3]);
		}
		for (int i = 16; i < 64; ++i)
		{
			const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
		std::uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
		for (int i = 0; i < 64; ++i)
		{
			const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
			const std::uint32_t ch = (e & f) ^ ((~e) & g);
			const std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
			const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
			const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			const std::uint32_t t2 = S0 + maj;
			hh = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		h_[0] += a;
		h_[1] += b;
		h_[2] += c;
		h_[3] += d;
		h_[4] += e;
		h_[5] += f;
		h_[6] += g;
		h_[7] += hh;
	}

	std::uint32_t h_[8];
	std::uint64_t nbits_;
	std::uint8_t buf_[64];
	std::size_t bufLen_;
};

std::string sha256HexOfFile(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) return {};

	Sha256Stream sha;
	char buf[8192];
	while (in)
	{
		in.read(buf, sizeof(buf));
		const auto n = in.gcount();
		if (n <= 0) break;
		sha.update(buf, static_cast<std::size_t>(n));
	}

	std::uint8_t digest[32];
	sha.final(digest);
	return digestToHex(digest, 32);
}

#endif

std::string toLowerCopy(std::string s)
{
	for (char& c: s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

bool envFlagSet(const char* name)
{
	const char* v = std::getenv(name);
	return v && v[0] != '\0' && v[0] != '0';
}

bool filenameLooksMultimodal(const std::string& modelPath)
{
	const std::string lower = toLowerCopy(modelPath);
	return lower.find("mmproj") != std::string::npos || lower.find("-vl-") != std::string::npos
		|| lower.find("_vl_") != std::string::npos;
}

// GGUF v2/v3 little-endian KV header (gguf.h at llama.cpp b10451).
// Type tags match enum gguf_type. Strings are u64 length + bytes, no NUL.
constexpr std::uint32_t kGgufTypeUint8 = 0;
constexpr std::uint32_t kGgufTypeInt8 = 1;
constexpr std::uint32_t kGgufTypeUint16 = 2;
constexpr std::uint32_t kGgufTypeInt16 = 3;
constexpr std::uint32_t kGgufTypeUint32 = 4;
constexpr std::uint32_t kGgufTypeInt32 = 5;
constexpr std::uint32_t kGgufTypeFloat32 = 6;
constexpr std::uint32_t kGgufTypeBool = 7;
constexpr std::uint32_t kGgufTypeString = 8;
constexpr std::uint32_t kGgufTypeArray = 9;
constexpr std::uint32_t kGgufTypeUint64 = 10;
constexpr std::uint32_t kGgufTypeInt64 = 11;
constexpr std::uint32_t kGgufTypeFloat64 = 12;

constexpr std::uint64_t kGgufMaxString = 1024 * 1024;
constexpr std::uint64_t kGgufMaxKv = 65536;
constexpr std::uint64_t kGgufMaxArray = 1024 * 1024;

std::size_t ggufScalarSize(std::uint32_t type)
{
	switch (type)
	{
		case kGgufTypeUint8:
		case kGgufTypeInt8:
		case kGgufTypeBool: return 1;
		case kGgufTypeUint16:
		case kGgufTypeInt16: return 2;
		case kGgufTypeUint32:
		case kGgufTypeInt32:
		case kGgufTypeFloat32: return 4;
		case kGgufTypeUint64:
		case kGgufTypeInt64:
		case kGgufTypeFloat64: return 8;
		default: return 0;
	}
}

class GgufByteReader
{
public:
	GgufByteReader(const std::uint8_t* data, std::size_t size)
		: data_(data)
		, size_(size)
	{}

	explicit GgufByteReader(std::ifstream& file)
		: file_(&file)
	{}

	bool read(void* dst, std::size_t n)
	{
		if (n == 0) return true;
		if (file_)
		{
			file_->read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
			return static_cast<std::size_t>(file_->gcount()) == n;
		}
		if (!data_ || pos_ + n > size_) return false;
		std::memcpy(dst, data_ + pos_, n);
		pos_ += n;
		return true;
	}

	bool skip(std::size_t n)
	{
		if (n == 0) return true;
		if (file_)
		{
			file_->seekg(static_cast<std::streamoff>(n), std::ios::cur);
			return static_cast<bool>(*file_);
		}
		if (pos_ + n > size_) return false;
		pos_ += n;
		return true;
	}

	template<typename T>
	bool readLe(T& out)
	{
		std::uint8_t buf[sizeof(T)];
		if (!read(buf, sizeof(T))) return false;
		T v = 0;
		for (std::size_t i = 0; i < sizeof(T); ++i)
			v |= static_cast<T>(buf[i]) << (8 * i);
		out = v;
		return true;
	}

	bool readString(std::string& out)
	{
		std::uint64_t len = 0;
		if (!readLe(len) || len > kGgufMaxString) return false;
		out.resize(static_cast<std::size_t>(len));
		if (len == 0) return true;
		return read(out.data(), static_cast<std::size_t>(len));
	}

private:
	const std::uint8_t* data_ = nullptr;
	std::size_t size_ = 0;
	std::size_t pos_ = 0;
	std::ifstream* file_ = nullptr;
};

bool skipGgufValue(GgufByteReader& r, std::uint32_t type)
{
	if (type == kGgufTypeString)
	{
		std::string tmp;
		return r.readString(tmp);
	}
	if (type == kGgufTypeArray)
	{
		std::uint32_t elemType = 0;
		std::uint64_t n = 0;
		if (!r.readLe(elemType) || !r.readLe(n) || n > kGgufMaxArray) return false;
		if (elemType == kGgufTypeString)
		{
			for (std::uint64_t i = 0; i < n; ++i)
			{
				std::string tmp;
				if (!r.readString(tmp)) return false;
			}
			return true;
		}
		const std::size_t elem = ggufScalarSize(elemType);
		if (elem == 0) return false;
		return r.skip(elem * static_cast<std::size_t>(n));
	}
	const std::size_t n = ggufScalarSize(type);
	if (n == 0) return false;
	return r.skip(n);
}

bool parseGgufKvStandalone(GgufByteReader& r, GgufIdentity& out)
{
	char magic[4];
	if (!r.read(magic, 4) || std::memcmp(magic, "GGUF", 4) != 0) return false;

	std::uint32_t version = 0;
	if (!r.readLe(version)) return false;
	if (version == 0 || version == 1 || (version & 0x0000FFFFu) == 0) return false;
	if (version > 3) return false;

	std::uint64_t nTensors = 0;
	std::uint64_t nKv = 0;
	if (!r.readLe(nTensors) || !r.readLe(nKv) || nKv > kGgufMaxKv) return false;

	std::string architecture;
	std::string name;
	for (std::uint64_t i = 0; i < nKv; ++i)
	{
		std::string key;
		std::uint32_t type = 0;
		if (!r.readString(key) || !r.readLe(type)) return false;
		if (type == kGgufTypeString && (key == "general.architecture" || key == "general.name"))
		{
			std::string val;
			if (!r.readString(val)) return false;
			if (key == "general.architecture") architecture = std::move(val);
			else name = std::move(val);
		}
		else if (!skipGgufValue(r, type))
		{
			return false;
		}
	}

	out.parsed = true;
	out.version = version;
	out.architecture = std::move(architecture);
	out.name = std::move(name);
	return true;
}

#if defined(RETDEC_HAS_LLAMACPP)

bool parseGgufKvLlamaBuffer(const void* data, std::size_t size, GgufIdentity& out)
{
	gguf_init_params params{};
	params.no_alloc = true;
	params.ctx = nullptr;
	gguf_context* ctx = gguf_init_from_buffer(data, size, params);
	if (!ctx) return false;

	out.parsed = true;
	out.version = gguf_get_version(ctx);
	const int64_t archId = gguf_find_key(ctx, "general.architecture");
	if (archId >= 0 && gguf_get_kv_type(ctx, archId) == GGUF_TYPE_STRING)
	{
		if (const char* s = gguf_get_val_str(ctx, archId)) out.architecture = s;
	}
	const int64_t nameId = gguf_find_key(ctx, "general.name");
	if (nameId >= 0 && gguf_get_kv_type(ctx, nameId) == GGUF_TYPE_STRING)
	{
		if (const char* s = gguf_get_val_str(ctx, nameId)) out.name = s;
	}
	gguf_free(ctx);
	return true;
}

bool parseGgufKvLlamaFile(const std::string& path, GgufIdentity& out)
{
	gguf_init_params params{};
	params.no_alloc = true;
	params.ctx = nullptr;
	gguf_context* ctx = gguf_init_from_file(path.c_str(), params);
	if (!ctx) return false;

	out.parsed = true;
	out.version = gguf_get_version(ctx);
	const int64_t archId = gguf_find_key(ctx, "general.architecture");
	if (archId >= 0 && gguf_get_kv_type(ctx, archId) == GGUF_TYPE_STRING)
	{
		if (const char* s = gguf_get_val_str(ctx, archId)) out.architecture = s;
	}
	const int64_t nameId = gguf_find_key(ctx, "general.name");
	if (nameId >= 0 && gguf_get_kv_type(ctx, nameId) == GGUF_TYPE_STRING)
	{
		if (const char* s = gguf_get_val_str(ctx, nameId)) out.name = s;
	}
	gguf_free(ctx);
	return true;
}

#endif

std::string modelsJsonPath()
{
	if (const char* override = std::getenv("RETDEC_NEURAL_MODELS_JSON"))
	{
		if (override[0]) return override;
	}
#ifdef RETDEC_NEURAL_DEFAULT_MODELS_JSON
	return RETDEC_NEURAL_DEFAULT_MODELS_JSON;
#else
	return "support/models.json";
#endif
}

std::set<std::string> loadAllowlistHashes()
{
	std::set<std::string> hashes;
	const std::string path = modelsJsonPath();
	std::ifstream in(path, std::ios::binary);
	if (!in) return hashes;

	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	rapidjson::Document doc;
	doc.Parse(text.c_str());
	if (doc.HasParseError() || !doc.IsObject()) return hashes;

	const auto models = doc.FindMember("models");
	if (models == doc.MemberEnd() || !models->value.IsArray()) return hashes;

	for (const auto& entry: models->value.GetArray())
	{
		if (!entry.IsObject()) continue;
		const auto sha = entry.FindMember("sha256");
		if (sha == entry.MemberEnd() || !sha->value.IsString()) continue;
		const std::string hex = toLowerCopy(sha->value.GetString());
		if (hex.size() == 64) hashes.insert(hex);
	}
	return hashes;
}

bool hashesMatch(const std::string& actual, const char* expected)
{
	if (!expected || !expected[0] || actual.empty()) return false;
	return actual == toLowerCopy(expected);
}

#if defined(RETDEC_NEURAL_HAS_OPENSSL)

std::string sha256HexOfMemory(const void* data, std::size_t size)
{
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x10100000L
	EVP_MD_CTX ctxStorage;
	EVP_MD_CTX* ctx = &ctxStorage;
	EVP_MD_CTX_init(ctx);
#else
	EVP_MD_CTX* ctx = EVP_MD_CTX_new();
	if (!ctx) return {};
#endif

	if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1)
	{
#if !defined(OPENSSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER >= 0x10100000L
		EVP_MD_CTX_free(ctx);
#else
		EVP_MD_CTX_cleanup(ctx);
#endif
		return {};
	}

	if (size > 0 && data
		&& EVP_DigestUpdate(ctx, data, size) != 1)
	{
#if !defined(OPENSSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER >= 0x10100000L
		EVP_MD_CTX_free(ctx);
#else
		EVP_MD_CTX_cleanup(ctx);
#endif
		return {};
	}

	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int len = 0;
	const int ok = EVP_DigestFinal_ex(ctx, digest, &len);
#if !defined(OPENSSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER >= 0x10100000L
	EVP_MD_CTX_free(ctx);
#else
	EVP_MD_CTX_cleanup(ctx);
#endif
	if (ok != 1 || len == 0) return {};
	return digestToHex(digest, len);
}

#else

std::string sha256HexOfMemory(const void* data, std::size_t size)
{
	Sha256Stream sha;
	if (data && size > 0) sha.update(data, size);
	std::uint8_t digest[32];
	sha.final(digest);
	return digestToHex(digest, 32);
}

#endif

} // namespace

std::string sha256HexOfBytes(const void* data, std::size_t size)
{
	return sha256HexOfMemory(data, size);
}

bool startsWithGgufMagic(const void* data, std::size_t size)
{
	return data && size >= 4 && std::memcmp(data, "GGUF", 4) == 0;
}

bool parseGgufIdentityFromMemory(const void* data, std::size_t size, GgufIdentity& out)
{
	out = GgufIdentity{};
	if (!startsWithGgufMagic(data, size)) return false;

#if defined(RETDEC_HAS_LLAMACPP)
	if (parseGgufKvLlamaBuffer(data, size, out)) return true;
	out = GgufIdentity{};
#endif
	GgufByteReader r(static_cast<const std::uint8_t*>(data), size);
	return parseGgufKvStandalone(r, out);
}

bool parseGgufIdentity(const std::string& modelPath, GgufIdentity& out)
{
	out = GgufIdentity{};
	if (modelPath.empty()) return false;

	std::ifstream peek(modelPath, std::ios::binary);
	char magic[4];
	if (!peek || !peek.read(magic, 4) || peek.gcount() != 4 || std::memcmp(magic, "GGUF", 4) != 0)
		return false;
	peek.close();

#if defined(RETDEC_HAS_LLAMACPP)
	if (parseGgufKvLlamaFile(modelPath, out)) return true;
	out = GgufIdentity{};
#endif
	std::ifstream in(modelPath, std::ios::binary);
	if (!in) return false;
	GgufByteReader r(in);
	return parseGgufKvStandalone(r, out);
}

bool ggufIdentityLooksMultimodal(const GgufIdentity& id)
{
	if (!id.parsed) return false;
	const std::string arch = toLowerCopy(id.architecture);
	const std::string name = toLowerCopy(id.name);
	if (arch.find("clip") != std::string::npos || arch.find("projector") != std::string::npos)
		return true;
	return name.find("mmproj") != std::string::npos;
}

bool verifyModelSha256(const std::string& modelPath)
{
	if (filenameLooksMultimodal(modelPath))
	{
		std::fprintf(stderr, "retdec-neural: refusing multimodal filename: %s\n", modelPath.c_str());
		return false;
	}

	GgufIdentity ident;
	if (parseGgufIdentity(modelPath, ident) && ggufIdentityLooksMultimodal(ident))
	{
		std::fprintf(
			stderr,
			"retdec-neural: refusing multimodal GGUF header (%s): architecture=%s name=%s\n",
			modelPath.c_str(),
			ident.architecture.c_str(),
			ident.name.c_str());
		return false;
	}

	const char* envSha = std::getenv("RETDEC_NEURAL_MODEL_SHA256");
	const bool haveEnv = envSha && envSha[0];
	const bool pinDefault = !haveEnv && namesMatchHint(modelPath);
	const bool unverified = envFlagSet("RETDEC_NEURAL_ALLOW_UNVERIFIED");

	const std::string actual = sha256HexOfFile(modelPath);

	if (haveEnv)
	{
		if (!hashesMatch(actual, envSha))
		{
			std::fprintf(
				stderr,
				"retdec-neural: SHA-256 mismatch for %s\n"
				"  expected %s\n"
				"  actual   %s\n",
				modelPath.c_str(),
				toLowerCopy(envSha).c_str(),
				actual.empty() ? "(unreadable)" : actual.c_str());
			return false;
		}
	}
	else if (pinDefault)
	{
		if (!hashesMatch(actual, kQwen35Q4KmSha256))
		{
			std::fprintf(
				stderr,
				"retdec-neural: SHA-256 mismatch for pinned Qwen filename %s\n"
				"  expected %s\n"
				"  actual   %s\n",
				modelPath.c_str(),
				kQwen35Q4KmSha256,
				actual.empty() ? "(unreadable)" : actual.c_str());
			return false;
		}
	}

	if (unverified) return true;

	if (actual.empty())
	{
		std::fprintf(
			stderr,
			"retdec-neural: cannot hash %s; unknown models are refused "
			"(set RETDEC_NEURAL_ALLOW_UNVERIFIED=1 or add sha256 to the allowlist)\n",
			modelPath.c_str());
		return false;
	}

	const auto allow = loadAllowlistHashes();
	if (allow.count(actual) == 0)
	{
		std::fprintf(
			stderr,
			"retdec-neural: %s hash %s is not in the model allowlist (%s)\n"
			"  add {\"name\":\"...\",\"sha256\":\"...\"} or set RETDEC_NEURAL_ALLOW_UNVERIFIED=1\n",
			modelPath.c_str(),
			actual.c_str(),
			modelsJsonPath().c_str());
		return false;
	}
	return true;
}

} // namespace retdec::neural
