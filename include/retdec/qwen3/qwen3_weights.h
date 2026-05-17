/**
 * @file include/retdec/qwen3/qwen3_weights.h
 * @brief Qwen3 weight loader — GGUF and SafeTensors formats.
 *
 * ## GGUF
 *
 * The GGUF (GGML Universal File) format stores model weights together with
 * metadata in a single binary file. The loader supports versions 1, 2, and 3
 * and handles all standard quantization types:
 *
 *   F32 · F16 · BF16 · Q8_0 · Q4_0 · Q4_1 · Q5_0 · Q5_1
 *   Q2_K · Q3_K_S/M/L · Q4_K_S/M · Q5_K_S/M · Q6_K · IQ*
 *
 * ## SafeTensors
 *
 * SafeTensors (Hugging Face) stores a JSON header (tensor metadata) followed
 * by raw tensor data in a single or sharded binary file.
 *
 * ## Usage
 *
 *   Qwen3Weights weights;
 *   if (!weights.openGguf("qwen3-4b-q4_k_m.gguf")) { ... }
 *
 *   // List all tensors
 *   for (auto& ti : weights.tensors())
 *       printf("%s  shape=[%u,%u]  dtype=%s\n",
 *              ti.name.c_str(), ti.dims[0], ti.dims[1],
 *              ggufDtypeName(ti.dtype));
 *
 *   // Load a single tensor
 *   auto tv = weights.load("token_embd.weight");
 *   if (tv) { const float* data = tv->dataF32(); ... }
 *
 *   // Config from GGUF metadata
 *   Qwen3Config cfg = weights.extractConfig();
 */

#ifndef RETDEC_QWEN3_WEIGHTS_H
#define RETDEC_QWEN3_WEIGHTS_H

#include "retdec/qwen3/qwen3_config.h"

#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace retdec::qwen3 {

// ─── GgufDtype ───────────────────────────────────────────────────────────────

enum class GgufDtype : uint32_t {
    F32    =  0,
    F16    =  1,
    Q4_0   =  2,
    Q4_1   =  3,
    // 4 and 5 are reserved
    Q5_0   =  6,
    Q5_1   =  7,
    Q8_0   =  8,
    Q8_1   =  9,
    Q2_K   = 10,
    Q3_K_S = 11,
    Q3_K_M = 12,
    Q3_K_L = 13,
    Q4_K_S = 14,
    Q4_K_M = 15,
    Q5_K_S = 16,
    Q5_K_M = 17,
    Q6_K   = 18,
    Q8_K   = 19,
    IQ2_XXS= 20,
    IQ2_XS = 21,
    IQ3_XXS= 22,
    IQ1_S  = 23,
    IQ4_NL = 24,
    IQ3_S  = 25,
    IQ2_S  = 26,
    IQ4_XS = 27,
    IQ1_M  = 28,
    BF16   = 32,
    Unknown= 0xFFFFFFFF,
};

const char* ggufDtypeName(GgufDtype dtype);
std::size_t ggufDtypeBlockSize(GgufDtype dtype);   ///< bytes per quant block
std::size_t ggufDtypeBlockElems(GgufDtype dtype);  ///< elements per quant block

// ─── GgufMetaValue ───────────────────────────────────────────────────────────

/// Typed value stored in GGUF key-value metadata.
using GgufMetaValue = std::variant<
    uint8_t, int8_t,
    uint16_t, int16_t,
    uint32_t, int32_t,
    uint64_t, int64_t,
    float, double,
    bool,
    std::string,
    std::vector<uint8_t>,   // array of uint8
    std::vector<int8_t>,
    std::vector<uint16_t>,
    std::vector<int16_t>,
    std::vector<uint32_t>,
    std::vector<int32_t>,
    std::vector<uint64_t>,
    std::vector<int64_t>,
    std::vector<float>,
    std::vector<double>,
    std::vector<std::string>
>;

// ─── TensorInfo ──────────────────────────────────────────────────────────────

/// Metadata about a single tensor in the file.
struct TensorInfo {
    std::string              name;
    uint32_t                 nDims = 0;
    std::array<uint64_t, 4>  dims  = {};    ///< dims[0] is innermost
    GgufDtype                dtype = GgufDtype::F32;
    uint64_t                 offset = 0;    ///< byte offset in data section
    uint64_t                 nElements = 0; ///< total element count

    uint64_t dataSizeBytes() const;
};

// ─── TensorView ──────────────────────────────────────────────────────────────

/**
 * @brief A loaded (or memory-mapped) tensor.
 *
 * The tensor data is stored in the file's native quantization format.
 * `dataRaw()` gives the raw bytes.  `dataF32()` dequantizes to float32
 * (returning a freshly allocated buffer); only supported for formats
 * the loader knows how to dequantize.
 */
class TensorView {
public:
    TensorView(TensorInfo info, std::vector<uint8_t> data);
    TensorView(TensorInfo info, const uint8_t* mapped, std::size_t size);

    /**
     * @brief Convenience constructor for building a TensorView from a raw byte
     *        buffer without a pre-existing TensorInfo.
     *
     * @param data    Raw quantized bytes (copied into the view).
     * @param dtype   Quantization format of the data.
     * @param shape   Tensor shape {d0, d1, ...} — total elements = product(shape).
     */
    TensorView(const std::vector<uint8_t>& data, GgufDtype dtype,
               std::vector<int64_t> shape);

    const TensorInfo& info() const { return info_; }

    /// Raw bytes in the native quantization format.
    const uint8_t* dataRaw()  const;
    std::size_t    rawBytes() const;

    /// Copy the raw bytes into a new std::vector<uint8_t>.
    std::vector<uint8_t> copyRawBytes() const;

    /// Dequantize to IEEE-754 float32 (allocates a new buffer).
    /// Returns nullopt if the dtype is not supported for dequantization.
    std::optional<std::vector<float>> dataF32() const;

    /**
     * @brief Dequantize a single row of a 2-D weight matrix to float32.
     *
     * Useful for embedding-table lookups without dequantizing the whole matrix.
     * The tensor must be 2-D: shape = [rows, cols].
     *
     * @param rowIdx  Zero-based row index.
     * @return        Dequantized row as float32 vector of length cols.
     *                Returns an empty vector if rowIdx is out of range or the
     *                tensor is not 2-D.
     */
    std::vector<float> row(int64_t rowIdx) const;

    /// Shape access helpers
    uint32_t nDims()              const { return info_.nDims; }
    uint64_t dim(uint32_t i)      const { return i < 4 ? info_.dims[i] : 1; }
    uint64_t nElements()          const { return info_.nElements; }
    GgufDtype dtype()             const { return info_.dtype; }

private:
    TensorInfo           info_;
    std::vector<uint8_t> ownedData_; ///< Non-empty if we own the buffer
    const uint8_t*       mapped_ = nullptr;
    std::size_t          mappedSize_ = 0;
};

// ─── SafeTensorsDtype ────────────────────────────────────────────────────────

enum class SafeTensorsDtype { F32, F16, BF16, I8, I16, I32, I64, U8, Unknown };
SafeTensorsDtype parseSafeTensorsDtype(std::string_view s);
const char*      safeTensorsDtypeName(SafeTensorsDtype dt);

// ─── SafeTensorInfo ──────────────────────────────────────────────────────────

struct SafeTensorInfo {
    std::string          name;
    SafeTensorsDtype     dtype  = SafeTensorsDtype::Unknown;
    std::vector<uint64_t> shape;
    uint64_t             dataOffsetBegin = 0; ///< Within the data section
    uint64_t             dataOffsetEnd   = 0;
    uint64_t             nElements()     const;
    uint64_t             dataSizeBytes() const;
};

// ─── Qwen3Weights ─────────────────────────────────────────────────────────────

/**
 * @brief Multi-format weight loader for Qwen3 models.
 *
 * Supports:
 *   - GGUF (single file, versions 1/2/3, all quant types)
 *   - SafeTensors (single or sharded `.safetensors` + `model.safetensors.index.json`)
 */
class Qwen3Weights {
public:
    Qwen3Weights();
    ~Qwen3Weights();

    // ── GGUF ────────────────────────────────────────────────────────────────

    /**
     * @brief Open a GGUF file and parse its header + tensor index.
     *
     * Does NOT load tensor data into memory (lazy).
     *
     * @param path  Filesystem path to the `.gguf` file.
     * @return true on success.
     */
    bool openGguf(const std::string& path);

    /**
     * @brief Check if a path is a valid GGUF file (magic check only).
     */
    static bool isGguf(const std::string& path);

    // ── SafeTensors ─────────────────────────────────────────────────────────

    /**
     * @brief Open a SafeTensors file or sharded index.
     *
     * If `path` ends in `.safetensors`, opens that single shard.
     * If `path` ends in `.json`, parses the shard index and registers all
     * referenced shard files (adjacent to the index).
     *
     * @return true on success.
     */
    bool openSafetensors(const std::string& path);

    /**
     * @brief Check if a path is a SafeTensors file (header magic).
     */
    static bool isSafetensors(const std::string& path);

    // ── Unified tensor access ────────────────────────────────────────────────

    /**
     * @brief List all tensors available in the opened file(s).
     */
    const std::vector<TensorInfo>& tensors() const { return ggufTensors_; }

    /**
     * @brief List all tensors from SafeTensors files.
     */
    const std::vector<SafeTensorInfo>& safeTensors() const { return stTensors_; }

    /**
     * @brief Load a single tensor by name from the opened GGUF file.
     *
     * Reads only the bytes for that tensor (supports memory mapping if
     * the platform provides it).
     *
     * @return Loaded TensorView, or nullopt if not found / load failed.
     */
    std::optional<TensorView> load(const std::string& name);

    /**
     * @brief Load a SafeTensors tensor by name.
     */
    std::optional<TensorView> loadSafetensor(const std::string& name);

    // ── Metadata ────────────────────────────────────────────────────────────

    /**
     * @brief Access GGUF metadata key-value store.
     */
    const std::unordered_map<std::string, GgufMetaValue>& metadata() const {
        return ggufMeta_;
    }

    /**
     * @brief Look up a single metadata key.
     */
    std::optional<GgufMetaValue> meta(const std::string& key) const;

    /**
     * @brief Return a string-array metadata value (or empty if not found).
     */
    std::vector<std::string>  metaStringArray(const std::string& key) const;

    /**
     * @brief Return a float-array metadata value (or empty if not found).
     */
    std::vector<float>        metaFloatArray(const std::string& key) const;

    /**
     * @brief Return an int32-array metadata value (or empty if not found).
     */
    std::vector<int32_t>      metaInt32Array(const std::string& key) const;

    /**
     * @brief Extract a Qwen3Config from GGUF metadata.
     *
     * Reads standard llama.cpp/gguf keys (llm.* namespace).
     */
    Qwen3Config extractConfig() const;

    // ── State ───────────────────────────────────────────────────────────────

    bool        isOpen()     const { return fileOpen_; }
    std::string format()     const { return format_; }  ///< "gguf" or "safetensors"
    const std::string& lastError() const { return lastError_; }

private:
    // ── GGUF parsing ──────────────────────────────────────────────────────────

    bool parseGgufHeader(std::FILE* f, uint32_t version);
    bool parseGgufMeta(std::FILE* f, uint32_t version, uint64_t nKv);
    bool parseGgufTensors(std::FILE* f, uint32_t version, uint64_t nTensors);
    GgufMetaValue readGgufValue(std::FILE* f, uint32_t type, uint32_t version);
    static uint64_t tensorNElems(const TensorInfo& ti);

    // ── SafeTensors parsing ───────────────────────────────────────────────────

    bool parseSafetensorsHeader(const std::string& path);
    bool parseSafetensorsIndex(const std::string& indexPath);

    // ── I/O helpers ───────────────────────────────────────────────────────────

    static std::string readGgufStr(std::FILE* f, uint32_t version);
    template<typename T> static T readLE(std::FILE* f);

    // ── Data ──────────────────────────────────────────────────────────────────

    bool        fileOpen_ = false;
    std::string format_;
    std::string filePath_;
    std::string lastError_;

    // GGUF
    uint64_t    ggufDataOffset_ = 0; ///< Byte offset where tensor data begins
    std::unordered_map<std::string, GgufMetaValue> ggufMeta_;
    std::vector<TensorInfo> ggufTensors_;
    std::unordered_map<std::string, std::size_t> ggufTensorIndex_;

    // SafeTensors
    struct SafetensorsShard {
        std::string path;
        uint64_t    headerSize = 0;
    };
    std::vector<SafeTensorInfo>    stTensors_;
    std::vector<SafetensorsShard>  stShards_;
    std::unordered_map<std::string, std::size_t> stTensorIndex_;
};

// ─── Dequantization helpers (public for testing) ─────────────────────────────

/// Dequantize Q8_0 block to float32 output (must have at least 32 floats)
void dequantQ8_0(const uint8_t* block, float* out);
/// Dequantize Q4_0 block to float32 output (must have at least 32 floats)
void dequantQ4_0(const uint8_t* block, float* out);
/// Dequantize Q4_1 block to float32 output (must have at least 32 floats)
void dequantQ4_1(const uint8_t* block, float* out);
/// Convert BF16 scalar to float32
float bf16ToF32(uint16_t v);
/// Convert F16 (IEEE-754 half) to float32
float f16ToF32(uint16_t v);

} // namespace retdec::qwen3

#endif // RETDEC_QWEN3_WEIGHTS_H
