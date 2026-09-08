/**
 * @file include/retdec/pyc_parser/py_marshal.h
 * @brief Python marshal format reader.
 *
 * ## Python marshal format overview
 *
 * Python's `marshal` module serialises objects for .pyc files.  The format
 * is a tagged binary stream:
 *
 * ```
 * object ::= flag? type_byte data
 * flag   ::= 0x80 or-ed into type_byte (FLAG_REF: add to reference table)
 * ```
 *
 * ## Type bytes
 *
 * | Byte | Name             | Description                                  |
 * |------|------------------|----------------------------------------------|
 * | '0'  | TYPE_NULL        | Used in dict keys to signal end               |
 * | 'N'  | TYPE_NONE        | None                                         |
 * | 'F'  | TYPE_FALSE       | False                                         |
 * | 'T'  | TYPE_TRUE        | True                                         |
 * | '.'  | TYPE_ELLIPSIS    | Ellipsis (...)                                |
 * | 'i'  | TYPE_INT         | 32-bit signed int (little-endian)             |
 * | 'l'  | TYPE_LONG        | Python long: n:int32 + n×uint16 digits       |
 * | 'f'  | TYPE_FLOAT       | ASCII float string (len:uint8 + bytes)        |
 * | 'g'  | TYPE_BINARY_FLOAT| 64-bit IEEE 754 double (little-endian)        |
 * | 'x'  | TYPE_COMPLEX     | Two ASCII float strings                       |
 * | 'y'  | TYPE_BINARY_COMPLEX | Two 64-bit doubles                         |
 * | 's'  | TYPE_STRING      | len:int32 + bytes (byte string, Python 2)     |
 * | 't'  | TYPE_INTERNED    | len:int32 + bytes (interned str, Python 2)    |
 * | 'R'  | TYPE_STRINGREF   | index:int32 into string table (Python 2)      |
 * | 'u'  | TYPE_UNICODE     | len:int32 + UTF-8 bytes (Python 3)            |
 * | 'A'  | TYPE_ASCII       | len:int32 + ASCII bytes (Python 3.4+)         |
 * | 'a'  | TYPE_ASCII_INTERNED | len:int32 + ASCII bytes, interned          |
 * | 'z'  | TYPE_SHORT_ASCII | len:uint8 + ASCII bytes                       |
 * | 'Z'  | TYPE_SHORT_ASCII_INTERNED | len:uint8 + ASCII bytes, interned   |
 * | 'B'  | TYPE_BYTES       | len:int32 + bytes (Python 3)                  |
 * | '('  | TYPE_TUPLE       | n:int32 + n objects                           |
 * | ')'  | TYPE_SMALL_TUPLE | n:uint8 + n objects                           |
 * | '['  | TYPE_LIST        | n:int32 + n objects                           |
 * | '{'  | TYPE_DICT        | alternating k, v until TYPE_NULL key          |
 * | '<'  | TYPE_SET         | n:int32 + n objects                           |
 * | '>'  | TYPE_FROZENSET   | n:int32 + n objects                           |
 * | 'c'  | TYPE_CODE        | Code object (version-dependent field order)   |
 * | 'r'  | TYPE_REF         | index:int32 into reference table              |
 *
 * ## FLAG_REF (0x80)
 *
 * If the high bit of the type byte is set, the object is added to the
 * reference table at its current index before deserialization. Later
 * TYPE_REF objects retrieve these by index.
 *
 * ## MarshalReader usage
 *
 * ```cpp
 * MarshalReader mr(data.data(), data.size(), version);
 * auto obj = mr.readObject();
 * if (obj && obj->type == MarshalObject::CODE)
 *     auto& code = std::get<std::shared_ptr<PyCodeObject>>(obj->value);
 * ```
 */

#ifndef RETDEC_PYC_PARSER_PY_MARSHAL_H
#define RETDEC_PYC_PARSER_PY_MARSHAL_H

#include "retdec/pyc_parser/py_code_object.h"
#include "retdec/pyc_parser/pyc_magic.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace retdec {
namespace pyc_parser {

// ─── MarshalObject ────────────────────────────────────────────────────────────

/**
 * @brief A deserialized Python marshal object.
 */
struct MarshalObject
{
	enum class Type
	{
		Null,
		None,
		True,
		False,
		Ellipsis,
		Int,
		Long,
		Float,
		Complex,
		Bytes,
		Str,
		Tuple,
		List,
		Set,
		FrozenSet,
		Dict,
		Code,
		Ref, ///< Already resolved during reading
	};

	using Value = std::variant<
		std::monostate,                              // Null / None / True / False / Ellipsis
		int64_t,                                     // Int / Long
		double,                                      // Float
		std::pair<double, double>,                   // Complex
		std::string,                                 // Bytes / Str
		std::vector<std::shared_ptr<MarshalObject>>, // Tuple / List / Set / FrozenSet
		std::vector<std::pair<
			std::shared_ptr<MarshalObject>,
			std::shared_ptr<MarshalObject>>>, // Dict
		std::shared_ptr<PyCodeObject>         // Code
		>;

	Type type = Type::None;
	Value value;

	bool isNone() const
	{
		return type == Type::None;
	}
	bool isTrue() const
	{
		return type == Type::True;
	}
	bool isFalse() const
	{
		return type == Type::False;
	}
	bool isInt() const
	{
		return type == Type::Int || type == Type::Long;
	}
	bool isFloat() const
	{
		return type == Type::Float;
	}
	bool isStr() const
	{
		return type == Type::Str;
	}
	bool isBytes() const
	{
		return type == Type::Bytes;
	}
	bool isTuple() const
	{
		return type == Type::Tuple;
	}
	bool isCode() const
	{
		return type == Type::Code;
	}

	int64_t asInt() const
	{
		return std::get<int64_t>(value);
	}
	double asFloat() const
	{
		return std::get<double>(value);
	}
	const std::string& asStr() const
	{
		return std::get<std::string>(value);
	}
	const std::vector<std::shared_ptr<MarshalObject>>& asTuple() const
	{
		return std::get<std::vector<std::shared_ptr<MarshalObject>>>(value);
	}
	const std::shared_ptr<PyCodeObject>& asCode() const
	{
		return std::get<std::shared_ptr<PyCodeObject>>(value);
	}

	/// Nodes one conversion may produce before it stops expanding.
	///
	/// FLAG_REF and TYPE_REF let a marshal stream describe a graph, while a
	/// PyCodeObject::Const is a tree, so a node reachable by two paths gets
	/// written out twice. Nesting that doubling k levels deep costs about
	/// seven bytes per level and yields 2^k nodes: a 176-byte stream used to
	/// exhaust 4 GB here. No bound derived from the stream length can catch
	/// that, so the expansion carries an absolute budget. At 96 bytes per node
	/// this caps one conversion at roughly 20 MB, still orders of magnitude
	/// above the constant pool of any real .pyc.
	static constexpr uint64_t kMaxConstNodes = 200000;

	/// Placed in the `sval` of a container whose elements were cut short by
	/// kMaxConstNodes, so a truncated tree cannot be read as a complete one.
	static constexpr const char* kConstTruncated = "<truncated: constant graph exceeds expansion limit>";

	/// Convert to PyCodeObject::Const (for storage in co_consts)
	PyCodeObject::Const toConst() const;

	/// Same conversion against a caller-owned budget, so that the constants of
	/// one code object share a single ceiling instead of one each.
	PyCodeObject::Const toConst(uint64_t& budget) const;

private:
	/// Convert the elements of a Tuple/FrozenSet into @p c under @p budget.
	void expandElements(PyCodeObject::Const& c, uint64_t& budget) const;
};

// ─── MarshalReader ────────────────────────────────────────────────────────────

/**
 * @brief Stateful reader for a Python marshal byte stream.
 *
 * Reads objects one at a time, maintaining an internal reference table
 * for FLAG_REF deduplication.
 */
class MarshalReader {
public:
	/// Elements reserved up front for a container, however many it declares.
	static constexpr size_t kInitialElementReserve = 64;

	/// Deepest container nesting accepted before a stream is called malformed.
	///
	/// CPython's own compiler does not emit anything close to this; the limit
	/// exists so a hostile file cannot turn two bytes per level into a stack
	/// overflow. Each level here is a few hundred bytes of frame, so 200 is
	/// comfortably inside any normal thread stack.
	static constexpr unsigned kMaxNestingDepth = 200;

	/// Width of one marshalled long digit; CPython stores longs in base 2^15.
	static constexpr size_t kLongDigitBits = 15;
	/// Bytes one such digit occupies on the wire (a little-endian uint16).
	static constexpr size_t kBytesPerLongDigit = 2;

	/**
	 * @brief Construct a reader for the given buffer.
	 *
	 * @param data     Pointer to the marshal stream start.
	 * @param size     Total bytes available.
	 * @param version  Python version (determines code object field order).
	 */
	MarshalReader(const uint8_t* data, size_t size, const PythonVersion& version);

	// ── Read API ──────────────────────────────────────────────────────────────

	/// Read the next object from the stream.
	/// Returns nullptr on EOF or error.
	/// Read the next object from the stream.
	///
	/// Nesting is bounded by kMaxNestingDepth. A marshal container costs only
	/// two bytes per level on the wire, so a 30 KB file can ask for 15,000
	/// levels of recursion and exhaust the stack -- no bound derived from the
	/// input size can catch that, which is why this one is a fixed depth.
	std::shared_ptr<MarshalObject> readObject();

	/// True if the entire buffer has been consumed.
	bool eof() const
	{
		return pos_ >= size_;
	}

	/// Current position in the stream.
	size_t pos() const
	{
		return pos_;
	}

	/// Set an error message.
	void setError(const std::string& msg)
	{
		error_ = msg;
		hasError_ = true;
	}

	/// True if a parse error occurred.
	bool hasError() const
	{
		return hasError_;
	}

	/// Return the most recent error message.
	const std::string& error() const
	{
		return error_;
	}

	// ── Primitive readers ─────────────────────────────────────────────────────

	bool readByte(uint8_t& out);
	bool readU16LE(uint16_t& out);
	bool readS32LE(int32_t& out);
	bool readU32LE(uint32_t& out);
	bool readF64LE(double& out);
	/// Rejects a declared container element count larger than the number of
	/// bytes left, which no well-formed stream can satisfy. See the definition.
	bool countFitsInRemainingInput(size_t n, const char* what, size_t bytesPerElement = 1);

	bool readBytes(std::vector<uint8_t>& out, size_t n);
	bool readString(std::string& out, size_t n); ///< n UTF-8 bytes
	bool readAscii(std::string& out, size_t n);  ///< n ASCII bytes
	bool readNullTerminatedFloat(double& out);   ///< TYPE_FLOAT format

	// ── Reference table ───────────────────────────────────────────────────────

	size_t reserveRef(); ///< Reserve a slot, return index
	void setRef(size_t idx, std::shared_ptr<MarshalObject> obj);
	std::shared_ptr<MarshalObject> getRef(size_t idx) const;
	size_t refCount() const
	{
		return refs_.size();
	}

	const PythonVersion& version() const
	{
		return version_;
	}

private:
	const uint8_t* data_;
	size_t size_;
	size_t pos_ = 0;
	PythonVersion version_;
	std::vector<std::shared_ptr<MarshalObject>> refs_;
	/// Current container nesting depth; see kMaxNestingDepth.
	unsigned depth_ = 0;

	bool hasError_ = false;
	std::string error_;

	// ── Type-specific readers ─────────────────────────────────────────────────

	std::shared_ptr<MarshalObject> readInt();
	std::shared_ptr<MarshalObject> readLong();
	std::shared_ptr<MarshalObject> readFloat(bool binary);
	std::shared_ptr<MarshalObject> readComplex(bool binary);
	std::shared_ptr<MarshalObject> readBytes();
	std::shared_ptr<MarshalObject> readStr(size_t len, bool ascii);
	std::shared_ptr<MarshalObject> readShortStr(bool ascii);
	std::shared_ptr<MarshalObject> readTuple(bool small);
	std::shared_ptr<MarshalObject> readList();
	std::shared_ptr<MarshalObject> readSet(bool frozen);
	std::shared_ptr<MarshalObject> readDict();
	std::shared_ptr<MarshalObject> readCode();
	std::shared_ptr<MarshalObject> readRef();

	// ── Code object field reader (version-specific) ───────────────────────────
	std::shared_ptr<PyCodeObject> readCodeFields();
};

} // namespace pyc_parser
} // namespace retdec

#endif // RETDEC_PYC_PARSER_PY_MARSHAL_H
