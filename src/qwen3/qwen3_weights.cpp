/**
 * @file src/qwen3/qwen3_weights.cpp
 * @brief Qwen3 weight loader — GGUF and SafeTensors implementation.
 *
 * ## GGUF binary layout (version 3):
 *
 *   [4]  magic  = "GGUF"
 *   [4]  version                          (uint32 LE)
 *   [8]  tensor_count                     (uint64 LE)
 *   [8]  metadata_kv_count                (uint64 LE)
 *   [n]  metadata_kv[]  (typed key-value pairs)
 *   [n]  tensor_infos[] (name, n_dims, dims[], type, offset)
 *   ---alignment padding to GGUF_DEFAULT_ALIGNMENT (default 32)---
 *   [n]  tensor data
 *
 * ## SafeTensors layout:
 *
 *   [8]  header_size                      (uint64 LE)
 *   [header_size]  JSON header
 *   [n]  raw tensor data (contiguous)
 */

#include "retdec/qwen3/qwen3_weights.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>

namespace retdec::qwen3 {

// ─── Helpers ──────────────────────────────────────────────────────────────────

template<typename T>
T Qwen3Weights::readLE(std::FILE* f) {
    T v{};
    (void)std::fread(&v, sizeof(T), 1, f);
    return v; // Assumes little-endian host (all modern x86/ARM)
}

static std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

// ─── GgufDtype helpers ────────────────────────────────────────────────────────

const char* ggufDtypeName(GgufDtype dt) {
    switch (dt) {
    case GgufDtype::F32:    return "F32";
    case GgufDtype::F16:    return "F16";
    case GgufDtype::Q4_0:   return "Q4_0";
    case GgufDtype::Q4_1:   return "Q4_1";
    case GgufDtype::Q5_0:   return "Q5_0";
    case GgufDtype::Q5_1:   return "Q5_1";
    case GgufDtype::Q8_0:   return "Q8_0";
    case GgufDtype::Q8_1:   return "Q8_1";
    case GgufDtype::Q2_K:   return "Q2_K";
    case GgufDtype::Q3_K_S: return "Q3_K_S";
    case GgufDtype::Q3_K_M: return "Q3_K_M";
    case GgufDtype::Q3_K_L: return "Q3_K_L";
    case GgufDtype::Q4_K_S: return "Q4_K_S";
    case GgufDtype::Q4_K_M: return "Q4_K_M";
    case GgufDtype::Q5_K_S: return "Q5_K_S";
    case GgufDtype::Q5_K_M: return "Q5_K_M";
    case GgufDtype::Q6_K:   return "Q6_K";
    case GgufDtype::Q8_K:   return "Q8_K";
    case GgufDtype::IQ2_XXS:return "IQ2_XXS";
    case GgufDtype::IQ2_XS: return "IQ2_XS";
    case GgufDtype::IQ3_XXS:return "IQ3_XXS";
    case GgufDtype::IQ1_S:  return "IQ1_S";
    case GgufDtype::IQ4_NL: return "IQ4_NL";
    case GgufDtype::IQ3_S:  return "IQ3_S";
    case GgufDtype::IQ2_S:  return "IQ2_S";
    case GgufDtype::IQ4_XS: return "IQ4_XS";
    case GgufDtype::IQ1_M:  return "IQ1_M";
    case GgufDtype::BF16:   return "BF16";
    default:                 return "Unknown";
    }
}

// Block sizes in bytes (one quantization block contains 32 elements except K-quants).
std::size_t ggufDtypeBlockSize(GgufDtype dt) {
    switch (dt) {
    case GgufDtype::F32:    return sizeof(float);
    case GgufDtype::F16:    return sizeof(uint16_t);
    case GgufDtype::BF16:   return sizeof(uint16_t);
    case GgufDtype::Q4_0:   return 2 + 16;      // scale(f16) + 16 bytes of nibbles
    case GgufDtype::Q4_1:   return 2 + 2 + 16;  // scale + min + nibbles
    case GgufDtype::Q5_0:   return 2 + 4 + 16;  // scale + qh + nibbles
    case GgufDtype::Q5_1:   return 2 + 2 + 4 + 16;
    case GgufDtype::Q8_0:   return 2 + 32;      // scale(f16) + 32 bytes
    case GgufDtype::Q8_1:   return 4 + 4 + 32;
    case GgufDtype::Q2_K:   return 256;
    case GgufDtype::Q3_K_S: return 110;
    case GgufDtype::Q3_K_M: return 138;
    case GgufDtype::Q3_K_L: return 166;
    case GgufDtype::Q4_K_S: return 144;
    case GgufDtype::Q4_K_M: return 144;
    case GgufDtype::Q5_K_S: return 176;
    case GgufDtype::Q5_K_M: return 176;
    case GgufDtype::Q6_K:   return 210;
    case GgufDtype::Q8_K:   return 292;
    default:                 return 1;
    }
}

std::size_t ggufDtypeBlockElems(GgufDtype dt) {
    switch (dt) {
    case GgufDtype::F32:
    case GgufDtype::F16:
    case GgufDtype::BF16:   return 1;
    case GgufDtype::Q8_K:   return 256;
    case GgufDtype::Q2_K:
    case GgufDtype::Q3_K_S:
    case GgufDtype::Q3_K_M:
    case GgufDtype::Q3_K_L:
    case GgufDtype::Q4_K_S:
    case GgufDtype::Q4_K_M:
    case GgufDtype::Q5_K_S:
    case GgufDtype::Q5_K_M:
    case GgufDtype::Q6_K:   return 256;
    default:                 return 32;
    }
}

// ─── TensorInfo ───────────────────────────────────────────────────────────────

uint64_t TensorInfo::dataSizeBytes() const {
    std::size_t bs = ggufDtypeBlockSize(dtype);
    std::size_t be = ggufDtypeBlockElems(dtype);
    if (be == 1) return nElements * bs;
    return ((nElements + be - 1) / be) * bs;
}

// ─── TensorView ───────────────────────────────────────────────────────────────

TensorView::TensorView(TensorInfo info, std::vector<uint8_t> data)
    : info_(std::move(info)), ownedData_(std::move(data)) {}

TensorView::TensorView(TensorInfo info, const uint8_t* mapped, std::size_t size)
    : info_(std::move(info)), mapped_(mapped), mappedSize_(size) {}

TensorView::TensorView(const std::vector<uint8_t>& data, GgufDtype dtype,
                        std::vector<int64_t> shape) {
    info_.dtype = dtype;
    info_.nDims = static_cast<uint32_t>(shape.size());
    info_.nElements = 1;
    for (int i = 0; i < static_cast<int>(shape.size()) && i < 4; ++i) {
        info_.dims[i] = shape[i];
        info_.nElements *= shape[i];
    }
    ownedData_ = data;
}

const uint8_t* TensorView::dataRaw() const {
    return ownedData_.empty() ? mapped_ : ownedData_.data();
}

std::size_t TensorView::rawBytes() const {
    return ownedData_.empty() ? mappedSize_ : ownedData_.size();
}

std::vector<uint8_t> TensorView::copyRawBytes() const {
    const uint8_t* p = dataRaw();
    std::size_t    n = rawBytes();
    return std::vector<uint8_t>(p, p + n);
}

std::vector<float> TensorView::row(int64_t rowIdx) const {
    if (info_.nDims < 1) return {};
    int64_t cols   = info_.dims[0];   // innermost = columns (GGUF row-major)
    int64_t rows   = (info_.nDims >= 2) ? info_.dims[1] : 1;
    if (rowIdx < 0 || rowIdx >= rows) return {};

    // Compute byte range of this row
    std::size_t bs  = ggufDtypeBlockSize(info_.dtype);
    std::size_t be  = ggufDtypeBlockElems(info_.dtype);
    std::size_t rowBytes;
    if (be == 1)
        rowBytes = cols * bs;
    else
        rowBytes = ((cols + be - 1) / be) * bs;

    const uint8_t* rowPtr = dataRaw() + rowIdx * rowBytes;
    std::vector<float> out(cols, 0.f);

    // Build a temporary TensorView for just this row and dequantize
    TensorInfo rowInfo;
    rowInfo.dtype     = info_.dtype;
    rowInfo.nDims     = 1;
    rowInfo.dims[0]   = cols;
    rowInfo.nElements = cols;
    TensorView rowView(rowInfo, rowPtr, rowBytes);
    auto f32 = rowView.dataF32();
    if (f32) return *f32;

    // Fallback for unsupported K-quant types: dequantize with ops kernel
    // (import avoid — just zero-fill; caller should use gemv instead)
    return out;
}

// Dequantize to F32
std::optional<std::vector<float>> TensorView::dataF32() const {
    const uint8_t* raw = dataRaw();
    if (!raw) return std::nullopt;

    std::vector<float> out;
    out.resize(static_cast<std::size_t>(info_.nElements));

    switch (info_.dtype) {
    case GgufDtype::F32:
        std::memcpy(out.data(), raw, out.size() * sizeof(float));
        break;

    case GgufDtype::F16:
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = f16ToF32(reinterpret_cast<const uint16_t*>(raw)[i]);
        break;

    case GgufDtype::BF16:
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = bf16ToF32(reinterpret_cast<const uint16_t*>(raw)[i]);
        break;

    case GgufDtype::Q8_0: {
        constexpr std::size_t kBlockElems = 32;
        constexpr std::size_t kBlockBytes = 2 + 32;
        std::size_t nBlocks = out.size() / kBlockElems;
        for (std::size_t b = 0; b < nBlocks; ++b)
            dequantQ8_0(raw + b * kBlockBytes,
                        out.data() + b * kBlockElems);
        break;
    }

    case GgufDtype::Q4_0: {
        constexpr std::size_t kBlockElems = 32;
        constexpr std::size_t kBlockBytes = 2 + 16;
        std::size_t nBlocks = out.size() / kBlockElems;
        for (std::size_t b = 0; b < nBlocks; ++b)
            dequantQ4_0(raw + b * kBlockBytes,
                        out.data() + b * kBlockElems);
        break;
    }

    case GgufDtype::Q4_1: {
        constexpr std::size_t kBlockElems = 32;
        constexpr std::size_t kBlockBytes = 4 + 16;
        std::size_t nBlocks = out.size() / kBlockElems;
        for (std::size_t b = 0; b < nBlocks; ++b)
            dequantQ4_1(raw + b * kBlockBytes,
                        out.data() + b * kBlockElems);
        break;
    }

    default:
        // Complex K-quant types: return nullopt; callers can handle
        return std::nullopt;
    }

    return out;
}

// ─── Dequantization helpers ───────────────────────────────────────────────────

float bf16ToF32(uint16_t v) {
    uint32_t f32bits = static_cast<uint32_t>(v) << 16;
    float result;
    std::memcpy(&result, &f32bits, sizeof(result));
    return result;
}

float f16ToF32(uint16_t h) {
    uint32_t s = (h & 0x8000u) << 16;
    uint32_t e = (h & 0x7C00u) >> 10;
    uint32_t m = (h & 0x03FFu);
    uint32_t f;
    if (e == 0) {
        if (m == 0) { f = s; }
        else { // denormal
            e = 1;
            while (!(m & 0x0400)) { m <<= 1; --e; }
            m &= 0x03FF;
            f = s | ((e + 112) << 23) | (m << 13);
        }
    } else if (e == 31) {
        f = s | 0x7F800000u | (m << 13); // Inf/NaN
    } else {
        f = s | ((e + 112) << 23) | (m << 13);
    }
    float result;
    std::memcpy(&result, &f, sizeof(result));
    return result;
}

// Q8_0: 2-byte scale (f16) + 32 signed bytes
void dequantQ8_0(const uint8_t* block, float* out) {
    uint16_t scaleRaw;
    std::memcpy(&scaleRaw, block, 2);
    float scale = f16ToF32(scaleRaw);
    const int8_t* qs = reinterpret_cast<const int8_t*>(block + 2);
    for (int i = 0; i < 32; ++i)
        out[i] = qs[i] * scale;
}

// Q4_0: 2-byte scale (f16) + 16 bytes (32 nibbles, each [-8,7])
void dequantQ4_0(const uint8_t* block, float* out) {
    uint16_t scaleRaw;
    std::memcpy(&scaleRaw, block, 2);
    float scale = f16ToF32(scaleRaw);
    const uint8_t* qs = block + 2;
    for (int i = 0; i < 16; ++i) {
        out[2*i    ] = (static_cast<int>(qs[i] & 0x0F) - 8) * scale;
        out[2*i + 1] = (static_cast<int>(qs[i] >> 4)   - 8) * scale;
    }
}

// Q4_1: 2-byte scale (f16) + 2-byte min (f16) + 16 bytes (32 nibbles, each [0,15])
void dequantQ4_1(const uint8_t* block, float* out) {
    uint16_t scaleRaw, minRaw;
    std::memcpy(&scaleRaw, block,     2);
    std::memcpy(&minRaw,   block + 2, 2);
    float scale = f16ToF32(scaleRaw);
    float minV  = f16ToF32(minRaw);
    const uint8_t* qs = block + 4;
    for (int i = 0; i < 16; ++i) {
        out[2*i    ] = (qs[i] & 0x0F) * scale + minV;
        out[2*i + 1] = (qs[i] >> 4)   * scale + minV;
    }
}

// ─── SafeTensorsDtype ─────────────────────────────────────────────────────────

SafeTensorsDtype parseSafeTensorsDtype(std::string_view s) {
    if (s == "F32")  return SafeTensorsDtype::F32;
    if (s == "F16")  return SafeTensorsDtype::F16;
    if (s == "BF16") return SafeTensorsDtype::BF16;
    if (s == "I8")   return SafeTensorsDtype::I8;
    if (s == "I16")  return SafeTensorsDtype::I16;
    if (s == "I32")  return SafeTensorsDtype::I32;
    if (s == "I64")  return SafeTensorsDtype::I64;
    if (s == "U8")   return SafeTensorsDtype::U8;
    return SafeTensorsDtype::Unknown;
}

const char* safeTensorsDtypeName(SafeTensorsDtype dt) {
    switch (dt) {
    case SafeTensorsDtype::F32:  return "F32";
    case SafeTensorsDtype::F16:  return "F16";
    case SafeTensorsDtype::BF16: return "BF16";
    case SafeTensorsDtype::I8:   return "I8";
    case SafeTensorsDtype::I16:  return "I16";
    case SafeTensorsDtype::I32:  return "I32";
    case SafeTensorsDtype::I64:  return "I64";
    case SafeTensorsDtype::U8:   return "U8";
    default:                     return "Unknown";
    }
}

uint64_t SafeTensorInfo::nElements() const {
    uint64_t n = 1;
    for (auto d : shape) n *= d;
    return n;
}

static std::size_t safetensorsDtypeBytes(SafeTensorsDtype dt) {
    switch (dt) {
    case SafeTensorsDtype::F32:
    case SafeTensorsDtype::I32:  return 4;
    case SafeTensorsDtype::F16:
    case SafeTensorsDtype::BF16:
    case SafeTensorsDtype::I16:  return 2;
    case SafeTensorsDtype::I64:  return 8;
    case SafeTensorsDtype::I8:
    case SafeTensorsDtype::U8:   return 1;
    default:                     return 1;
    }
}

uint64_t SafeTensorInfo::dataSizeBytes() const {
    return nElements() * safetensorsDtypeBytes(dtype);
}

// ─── Qwen3Weights construction ────────────────────────────────────────────────

Qwen3Weights::Qwen3Weights()  = default;
Qwen3Weights::~Qwen3Weights() = default;

// ─── isGguf ──────────────────────────────────────────────────────────────────

bool Qwen3Weights::isGguf(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t magic = 0;
    (void)std::fread(&magic, 4, 1, f);
    std::fclose(f);
    return magic == 0x46554747u; // "GGUF" little-endian
}

// ─── GGUF string reading ──────────────────────────────────────────────────────

std::string Qwen3Weights::readGgufStr(std::FILE* f, uint32_t /*version*/) {
    uint64_t len = readLE<uint64_t>(f);
    if (len == 0 || len > 4'000'000) return {};
    std::string s(len, '\0');
    (void)std::fread(s.data(), 1, len, f);
    return s;
}

// ─── GGUF metadata value reading ─────────────────────────────────────────────

GgufMetaValue Qwen3Weights::readGgufValue(std::FILE* f,
                                            uint32_t type,
                                            uint32_t version) {
    // GGUF value types:
    //  0=uint8 1=int8 2=uint16 3=int16 4=uint32 5=int32 6=float32
    //  7=bool  8=string 9=array 10=uint64 11=int64 12=float64
    switch (type) {
    case 0:  return readLE<uint8_t>(f);
    case 1:  return readLE<int8_t>(f);
    case 2:  return readLE<uint16_t>(f);
    case 3:  return readLE<int16_t>(f);
    case 4:  return readLE<uint32_t>(f);
    case 5:  return readLE<int32_t>(f);
    case 6:  return readLE<float>(f);
    case 7:  { uint8_t b = readLE<uint8_t>(f); return b != 0; }
    case 8:  return readGgufStr(f, version);
    case 10: return readLE<uint64_t>(f);
    case 11: return readLE<int64_t>(f);
    case 12: return readLE<double>(f);
    case 9:  {
        // Array: elem_type (uint32) + count (uint64) + elements
        uint32_t elemType = readLE<uint32_t>(f);
        uint64_t count    = readLE<uint64_t>(f);
        if (count > 1'000'000) return std::vector<uint32_t>{};
        switch (elemType) {
        case 0:  { std::vector<uint8_t> v(count); for(auto&x:v)x=readLE<uint8_t>(f);  return v; }
        case 1:  { std::vector<int8_t>  v(count); for(auto&x:v)x=readLE<int8_t>(f);   return v; }
        case 2:  { std::vector<uint16_t>v(count); for(auto&x:v)x=readLE<uint16_t>(f); return v; }
        case 3:  { std::vector<int16_t> v(count); for(auto&x:v)x=readLE<int16_t>(f);  return v; }
        case 4:  { std::vector<uint32_t>v(count); for(auto&x:v)x=readLE<uint32_t>(f); return v; }
        case 5:  { std::vector<int32_t> v(count); for(auto&x:v)x=readLE<int32_t>(f);  return v; }
        case 6:  { std::vector<float>   v(count); for(auto&x:v)x=readLE<float>(f);    return v; }
        case 8:  { std::vector<std::string> v; v.reserve(count);
                   for(uint64_t i=0;i<count;++i) v.push_back(readGgufStr(f,version));
                   return v; }
        case 10: { std::vector<uint64_t>v(count); for(auto&x:v)x=readLE<uint64_t>(f); return v; }
        case 11: { std::vector<int64_t> v(count); for(auto&x:v)x=readLE<int64_t>(f);  return v; }
        case 12: { std::vector<double>  v(count); for(auto&x:v)x=readLE<double>(f);   return v; }
        default: { // Skip unknown element types
            for(uint64_t i=0;i<count;++i) readLE<uint32_t>(f);
            return std::vector<uint32_t>{};
        }
        }
    }
    default:
        return uint32_t(0);
    }
}

// ─── openGguf ─────────────────────────────────────────────────────────────────

bool Qwen3Weights::openGguf(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        lastError_ = "Cannot open file: " + path;
        return false;
    }

    // Magic + version
    uint32_t magic   = readLE<uint32_t>(f);
    uint32_t version = readLE<uint32_t>(f);

    if (magic != 0x46554747u) { // "GGUF"
        lastError_ = "Not a GGUF file (bad magic)";
        std::fclose(f);
        return false;
    }
    if (version < 1 || version > 3) {
        lastError_ = "Unsupported GGUF version: " + std::to_string(version);
        std::fclose(f);
        return false;
    }

    uint64_t nTensors = readLE<uint64_t>(f);
    uint64_t nKv      = readLE<uint64_t>(f);

    // Metadata
    for (uint64_t i = 0; i < nKv; ++i) {
        std::string key   = readGgufStr(f, version);
        uint32_t    vtype = readLE<uint32_t>(f);
        GgufMetaValue val = readGgufValue(f, vtype, version);
        ggufMeta_[key] = std::move(val);
    }

    // Tensor index
    ggufTensors_.reserve(static_cast<std::size_t>(nTensors));
    for (uint64_t i = 0; i < nTensors; ++i) {
        TensorInfo ti;
        ti.name   = readGgufStr(f, version);
        ti.nDims  = readLE<uint32_t>(f);
        if (ti.nDims > 4) ti.nDims = 4;
        ti.dims.fill(1);
        ti.nElements = 1;
        for (uint32_t d = 0; d < ti.nDims; ++d) {
            ti.dims[d]    = readLE<uint64_t>(f);
            ti.nElements *= ti.dims[d];
        }
        uint32_t dtypeRaw = readLE<uint32_t>(f);
        ti.dtype          = static_cast<GgufDtype>(dtypeRaw);
        ti.offset         = readLE<uint64_t>(f);

        std::size_t idx = ggufTensors_.size();
        ggufTensorIndex_[ti.name] = idx;
        ggufTensors_.push_back(std::move(ti));
    }

    // Data section starts at the next alignment boundary (default 32)
    constexpr uint64_t kAlign = 32;
    long pos = std::ftell(f);
    ggufDataOffset_ = (static_cast<uint64_t>(pos) + kAlign - 1) & ~(kAlign - 1);

    std::fclose(f);
    fileOpen_  = true;
    format_    = "gguf";
    filePath_  = path;
    return true;
}

// ─── load (GGUF tensor) ───────────────────────────────────────────────────────

std::optional<TensorView> Qwen3Weights::load(const std::string& name) {
    auto it = ggufTensorIndex_.find(name);
    if (it == ggufTensorIndex_.end()) {
        lastError_ = "Tensor not found: " + name;
        return std::nullopt;
    }
    const TensorInfo& ti = ggufTensors_[it->second];
    uint64_t byteSize    = ti.dataSizeBytes();

    std::FILE* f = std::fopen(filePath_.c_str(), "rb");
    if (!f) {
        lastError_ = "Cannot reopen file: " + filePath_;
        return std::nullopt;
    }

    uint64_t absOffset = ggufDataOffset_ + ti.offset;
    if (std::fseek(f, static_cast<long>(absOffset), SEEK_SET) != 0) {
        std::fclose(f);
        lastError_ = "Seek failed for tensor: " + name;
        return std::nullopt;
    }

    std::vector<uint8_t> data(static_cast<std::size_t>(byteSize));
    std::size_t read = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);

    if (read != data.size()) {
        lastError_ = "Short read for tensor: " + name;
        return std::nullopt;
    }

    return TensorView(ti, std::move(data));
}

// ─── extractConfig ────────────────────────────────────────────────────────────

std::optional<GgufMetaValue> Qwen3Weights::meta(const std::string& key) const {
    auto it = ggufMeta_.find(key);
    if (it == ggufMeta_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::string> Qwen3Weights::metaStringArray(const std::string& key) const {
    auto v = meta(key);
    if (!v) return {};
    if (auto* p = std::get_if<std::vector<std::string>>(&*v)) return *p;
    return {};
}

std::vector<float> Qwen3Weights::metaFloatArray(const std::string& key) const {
    auto v = meta(key);
    if (!v) return {};
    if (auto* p = std::get_if<std::vector<float>>(&*v)) return *p;
    // Some files store scores as doubles
    if (auto* p = std::get_if<std::vector<double>>(&*v)) {
        std::vector<float> out(p->size());
        for (std::size_t i = 0; i < p->size(); ++i) out[i] = static_cast<float>((*p)[i]);
        return out;
    }
    return {};
}

std::vector<int32_t> Qwen3Weights::metaInt32Array(const std::string& key) const {
    auto v = meta(key);
    if (!v) return {};
    if (auto* p = std::get_if<std::vector<int32_t>>(&*v)) return *p;
    if (auto* p = std::get_if<std::vector<uint32_t>>(&*v)) {
        std::vector<int32_t> out(p->size());
        for (std::size_t i = 0; i < p->size(); ++i)
            out[i] = static_cast<int32_t>((*p)[i]);
        return out;
    }
    if (auto* p = std::get_if<std::vector<int8_t>>(&*v)) {
        std::vector<int32_t> out(p->size());
        for (std::size_t i = 0; i < p->size(); ++i)
            out[i] = static_cast<int32_t>((*p)[i]);
        return out;
    }
    return {};
}

Qwen3Config Qwen3Weights::extractConfig() const {
    Qwen3Config c;

    // Helper to retrieve typed metadata values
    auto getU32 = [this](const std::string& k, uint32_t def) -> uint32_t {
        auto v = meta(k);
        if (!v) return def;
        if (auto* p = std::get_if<uint32_t>(&*v)) return *p;
        if (auto* p = std::get_if<int32_t>(&*v))  return static_cast<uint32_t>(*p);
        if (auto* p = std::get_if<uint64_t>(&*v)) return static_cast<uint32_t>(*p);
        return def;
    };
    auto getF32 = [this](const std::string& k, float def) -> float {
        auto v = meta(k);
        if (!v) return def;
        if (auto* p = std::get_if<float>(&*v))  return *p;
        if (auto* p = std::get_if<double>(&*v)) return static_cast<float>(*p);
        return def;
    };
    auto getStr = [this](const std::string& k, std::string def) -> std::string {
        auto v = meta(k);
        if (!v) return def;
        if (auto* p = std::get_if<std::string>(&*v)) return *p;
        return def;
    };

    // GGUF standard keys for LLM architectures (llama.cpp naming convention)
    c.vocabSize            = getU32("llm.vocab_size",                   c.vocabSize);
    c.hiddenSize           = getU32("llm.embedding_length",             c.hiddenSize);
    c.numLayers            = getU32("llm.block_count",                  c.numLayers);
    c.numHeads             = getU32("llm.attention.head_count",         c.numHeads);
    c.numKvHeads           = getU32("llm.attention.head_count_kv",      c.numKvHeads);
    c.intermediateSize     = getU32("llm.feed_forward_length",          c.intermediateSize);
    c.maxPositionEmbeddings= getU32("llm.context_length",               c.maxPositionEmbeddings);
    c.ropeTheta            = getF32("llm.rope.freq_base",               c.ropeTheta);
    c.numExperts           = getU32("llm.expert_count",                 c.numExperts);
    c.numExpertsPerTok     = getU32("llm.expert_used_count",            c.numExpertsPerTok);
    c.archName             = getStr("general.architecture",             c.archName);
    c.fileType             = getStr("general.file_type",                c.fileType);

    if (c.numHeads > 0)
        c.headDim = c.hiddenSize / c.numHeads;

    // Also check "tokenizer.ggml.eos_token_id"
    auto eos = getU32("tokenizer.ggml.eos_token_id", 0);
    if (eos != 0) c.eosTokenId = eos;

    return c;
}

// ─── SafeTensors loading ──────────────────────────────────────────────────────

bool Qwen3Weights::isSafetensors(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    // SafeTensors starts with an 8-byte little-endian header length,
    // then '{'. The header length must be sane (< 100 MB).
    uint64_t hdr = 0;
    if (std::fread(&hdr, 8, 1, f) != 1) { std::fclose(f); return false; }
    std::fclose(f);
    return hdr > 0 && hdr < 100'000'000;
}

// Minimal JSON parser for SafeTensors header
// Expected format: { "tensor_name": {"dtype":"F32","shape":[a,b],"data_offsets":[s,e]}, ... }
bool Qwen3Weights::parseSafetensorsHeader(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { lastError_ = "Cannot open: " + path; return false; }

    uint64_t headerLen = 0;
    if (std::fread(&headerLen, 8, 1, f) != 1 ||
        headerLen == 0 || headerLen > 50'000'000) {
        std::fclose(f);
        lastError_ = "Invalid SafeTensors header size";
        return false;
    }

    std::string hdr(static_cast<std::size_t>(headerLen), '\0');
    if (std::fread(hdr.data(), 1, hdr.size(), f) != hdr.size()) {
        std::fclose(f);
        lastError_ = "Cannot read SafeTensors header";
        return false;
    }
    std::fclose(f);

    uint64_t dataStart = 8 + headerLen;

    // Parse the JSON header
    // Very simplified: iterate top-level keys, skip "__metadata__"
    std::size_t pos = 0;
    auto skipWs = [&]() {
        while (pos < hdr.size() && (hdr[pos]==' '||hdr[pos]=='\n'||hdr[pos]=='\r'||hdr[pos]=='\t'))
            ++pos;
    };
    auto expect = [&](char c) { skipWs(); if(pos<hdr.size()&&hdr[pos]==c)++pos; };
    auto readStr = [&]() {
        expect('"');
        std::string r;
        while(pos<hdr.size()&&hdr[pos]!='"'){if(hdr[pos]=='\\'&&pos+1<hdr.size()){pos++;r+=hdr[pos];}else{r+=hdr[pos];}++pos;}
        if(pos<hdr.size())++pos;
        return r;
    };
    auto readInt = [&]() -> uint64_t {
        skipWs();
        uint64_t v=0;
        while(pos<hdr.size()&&hdr[pos]>='0'&&hdr[pos]<='9')v=v*10+(hdr[pos++]-'0');
        return v;
    };

    expect('{');
    while (pos < hdr.size()) {
        skipWs();
        if (pos < hdr.size() && hdr[pos] == '}') break;
        std::string name = readStr();
        expect(':');
        skipWs();

        if (name == "__metadata__") {
            // Skip to matching '}'
            int depth = 0;
            while (pos < hdr.size()) {
                if (hdr[pos]=='{') ++depth;
                else if (hdr[pos]=='}') { if(--depth<=0){++pos;break;} }
                ++pos;
            }
        } else {
            SafeTensorInfo ti;
            ti.name = name;
            expect('{');
            while (pos < hdr.size()) {
                skipWs();
                if (hdr[pos] == '}') { ++pos; break; }
                std::string fkey = readStr();
                expect(':');
                if (fkey == "dtype") {
                    std::string dtStr = readStr();
                    ti.dtype = parseSafeTensorsDtype(dtStr);
                } else if (fkey == "shape") {
                    expect('[');
                    while(pos<hdr.size()&&hdr[pos]!=']') {
                        skipWs();
                        if(hdr[pos]==']') break;
                        ti.shape.push_back(readInt());
                        skipWs();
                        if(pos<hdr.size()&&hdr[pos]==',')++pos;
                    }
                    expect(']');
                } else if (fkey == "data_offsets") {
                    expect('[');
                    skipWs(); ti.dataOffsetBegin = readInt();
                    skipWs(); if(pos<hdr.size()&&hdr[pos]==',')++pos;
                    skipWs(); ti.dataOffsetEnd   = readInt();
                    skipWs(); expect(']');
                } else {
                    // skip value
                    while(pos<hdr.size()&&hdr[pos]!=','&&hdr[pos]!='}')++pos;
                }
                skipWs();
                if(pos<hdr.size()&&hdr[pos]==',')++pos;
            }
            ti.dataOffsetBegin += dataStart;
            ti.dataOffsetEnd   += dataStart;
            std::size_t idx = stTensors_.size();
            stTensorIndex_[name] = idx;
            stTensors_.push_back(std::move(ti));
        }
        skipWs();
        if (pos < hdr.size() && hdr[pos] == ',') ++pos;
    }

    return true;
}

bool Qwen3Weights::openSafetensors(const std::string& path) {
    if (!parseSafetensorsHeader(path)) return false;

    SafetensorsShard shard;
    shard.path = path;
    stShards_.push_back(shard);

    fileOpen_ = true;
    format_   = "safetensors";
    filePath_ = path;
    return true;
}

std::optional<TensorView> Qwen3Weights::loadSafetensor(const std::string& name) {
    auto it = stTensorIndex_.find(name);
    if (it == stTensorIndex_.end()) {
        lastError_ = "SafeTensor not found: " + name;
        return std::nullopt;
    }
    const SafeTensorInfo& si = stTensors_[it->second];
    uint64_t byteSize = si.dataOffsetEnd - si.dataOffsetBegin;

    std::FILE* f = std::fopen(filePath_.c_str(), "rb");
    if (!f) { lastError_ = "Cannot open: " + filePath_; return std::nullopt; }
    if (std::fseek(f, static_cast<long>(si.dataOffsetBegin), SEEK_SET)) {
        std::fclose(f);
        lastError_ = "Seek failed for: " + name;
        return std::nullopt;
    }

    std::vector<uint8_t> data(static_cast<std::size_t>(byteSize));
    std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);

    // Construct a TensorInfo from SafeTensorInfo for the TensorView
    TensorInfo ti;
    ti.name     = si.name;
    ti.nDims    = static_cast<uint32_t>(si.shape.size());
    ti.nElements = si.nElements();
    ti.dims.fill(1);
    for (uint32_t d = 0; d < ti.nDims && d < 4; ++d)
        ti.dims[d] = si.shape[d];
    // Map SafeTensorsDtype → GgufDtype for TensorView
    switch (si.dtype) {
    case SafeTensorsDtype::F32:  ti.dtype = GgufDtype::F32;  break;
    case SafeTensorsDtype::F16:  ti.dtype = GgufDtype::F16;  break;
    case SafeTensorsDtype::BF16: ti.dtype = GgufDtype::BF16; break;
    default:                     ti.dtype = GgufDtype::Unknown; break;
    }

    return TensorView(ti, std::move(data));
}

} // namespace retdec::qwen3
