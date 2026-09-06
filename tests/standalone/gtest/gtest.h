/**
 * @file tests/standalone/gtest/gtest.h
 * @brief Dependency-free GoogleTest-compatible shim ("gtest-lite").
 *
 * Purpose
 * -------
 * The full RetDec build needs an LLVM source build (hours, tens of GB) plus
 * network downloads of Capstone/Keystone/YARA/GoogleTest before a single unit
 * test can run.  57 of the src/ modules — the whole Imortek detector, SSA,
 * codegen and type layer — depend only on the in-house `retdec/ssa` IR and the
 * C++17 standard library; they compile with nothing but
 * `g++ -std=c++17 -Iinclude`.
 *
 * This header lets those modules' existing GoogleTest suites build and run with
 * no dependencies at all, so a contributor, CI job or reviewer can verify the
 * detector layer in seconds instead of hours.  See `scripts/standalone_check.sh`.
 *
 * Supported surface (the subset the LLVM-free suites actually use):
 *   TEST, TEST_F, TEST_P, INSTANTIATE_TEST_SUITE_P
 *   ::testing::Test, ::testing::TestWithParam<T>, ::testing::Values,
 *   ::testing::ValuesIn, ::testing::InitGoogleTest, RUN_ALL_TESTS
 *   EXPECT_/ASSERT_ x {TRUE,FALSE,EQ,NE,LT,LE,GT,GE,STREQ,STRNE,
 *                      NEAR,FLOAT_EQ,DOUBLE_EQ,NO_THROW,THROW,ANY_THROW}
 *   FAIL, SUCCEED, ADD_FAILURE, GTEST_SKIP
 *   --gtest_filter, --gtest_list_tests, --gtest_repeat, --gtest_brief
 *
 * Deliberately NOT supported: gmock, death tests, typed tests, sharding, XML
 * output.  Suites needing those keep using real GoogleTest via CMake.
 *
 * This is a test-only compatibility layer; it never ships in a release binary.
 */

#ifndef RETDEC_TESTS_STANDALONE_GTEST_LITE_H
#define RETDEC_TESTS_STANDALONE_GTEST_LITE_H

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace testing {
namespace lite {

// ─── Value printing ──────────────────────────────────────────────────────────
//
// The non-template overloads must be declared before the template, otherwise
// unqualified lookup from the template's definition context cannot see them
// (`const char*` and `std::string` bring no associated namespace for ADL).

std::string printValue(const char* v);
std::string printValue(char* v);
std::string printValue(const std::string& v);
std::string printValue(std::nullptr_t);

template <typename T, typename = void>
struct IsStreamable : std::false_type {};

template <typename T>
struct IsStreamable<T,
	std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
	: std::true_type {};

template <typename T>
std::string printValue(const T& v)
{
	if constexpr (std::is_same_v<T, bool>)
	{
		return v ? "true" : "false";
	}
	else if constexpr (std::is_enum_v<T>)
	{
		std::ostringstream os;
		os << static_cast<typename std::underlying_type<T>::type>(v);
		return os.str();
	}
	else if constexpr (IsStreamable<T>::value)
	{
		std::ostringstream os;
		os << v;
		return os.str();
	}
	else
	{
		return "<" + std::to_string(sizeof(T)) + "-byte object>";
	}
}

inline std::string printValue(const char* v)
{
	return v ? std::string("\"") + v + "\"" : std::string("NULL");
}
inline std::string printValue(char* v) { return printValue(static_cast<const char*>(v)); }
inline std::string printValue(const std::string& v) { return "\"" + v + "\""; }
inline std::string printValue(std::nullptr_t) { return "nullptr"; }

// ─── Message ─────────────────────────────────────────────────────────────────

class Message {
public:
	template <typename T>
	Message& operator<<(const T& v)
	{
		if constexpr (std::is_same_v<T, bool>) os_ << (v ? "true" : "false");
		else os_ << v;
		return *this;
	}
	Message& operator<<(std::ostream& (*m)(std::ostream&)) { os_ << m; return *this; }
	std::string str() const { return os_.str(); }

private:
	std::ostringstream os_;
};

// ─── Assertion result ────────────────────────────────────────────────────────

class Result {
public:
	Result() = default;
	Result(bool ok, std::string detail): ok_(ok), detail_(std::move(detail)) {}
	explicit operator bool() const { return ok_; }
	const std::string& detail() const { return detail_; }

private:
	bool ok_ = true;
	std::string detail_;
};

// ─── Per-test state ──────────────────────────────────────────────────────────

struct State {
	bool failed = false;
	bool skipped = false;
	std::string skipReason;
	std::vector<std::string> failures;
};

inline State& state()
{
	static State s;
	return s;
}

/// `AssertHelper(...) = Message() << ...`.  `operator=` returns void so a fatal
/// assertion can be written `return AssertHelper(...) = Message();` inside a
/// void test body — the same trick real GoogleTest uses to abort a test.
class AssertHelper {
public:
	AssertHelper(const Result& r, bool fatal, const char* file, int line)
		: detail_(r.detail()), fatal_(fatal), file_(file), line_(line) {}

	void operator=(const Message& extra) const
	{
		std::ostringstream os;
		os << file_ << ":" << line_ << (fatal_ ? "  [fatal]" : "") << "\n  " << detail_;
		const std::string e = extra.str();
		if (!e.empty()) os << "\n  " << e;
		state().failed = true;
		state().failures.push_back(os.str());
	}

private:
	std::string detail_;
	bool fatal_;
	const char* file_;
	int line_;
};

class SkipHelper {
public:
	SkipHelper(const char* file, int line): file_(file), line_(line) {}
	void operator=(const Message& extra) const
	{
		state().skipped = true;
		state().skipReason = extra.str().empty()
			? std::string(file_) + ":" + std::to_string(line_)
			: extra.str();
	}

private:
	const char* file_;
	int line_;
};

// ─── Comparators ─────────────────────────────────────────────────────────────

/// Comparing two `const char*` with `==` compares pointers, which is almost
/// never what a test means.  GoogleTest special-cases it; so do we.
template <typename A, typename B>
bool eqCompare(const A& a, const B& b)
{
	using DA = std::decay_t<A>;
	using DB = std::decay_t<B>;
	if constexpr ((std::is_same_v<DA, const char*> || std::is_same_v<DA, char*>)
		&& (std::is_same_v<DB, const char*> || std::is_same_v<DB, char*>))
	{
		if (a == nullptr || b == nullptr)
			return static_cast<const void*>(a) == static_cast<const void*>(b);
		return std::strcmp(a, b) == 0;
	}
	else
	{
		return a == b;
	}
}

template <typename A, typename B>
Result cmpEq(const A& a, const B& b, const char* ta, const char* tb)
{
	if (eqCompare(a, b)) return Result(true, "");
	return Result(false,
		std::string("Expected equality of these values:\n    ") + ta
		+ "\n      Which is: " + printValue(a)
		+ "\n    " + tb + "\n      Which is: " + printValue(b));
}

template <typename A, typename B>
Result cmpNe(const A& a, const B& b, const char* ta, const char* tb)
{
	if (!eqCompare(a, b)) return Result(true, "");
	return Result(false,
		std::string("Expected: (") + ta + ") != (" + tb
		+ "), actual: both are " + printValue(a));
}

#define RETDEC_GTL_REL_CMP(fnName, op, opText)                                     \
	template <typename A, typename B>                                              \
	Result fnName(const A& a, const B& b, const char* ta, const char* tb)          \
	{                                                                              \
		if (a op b) return Result(true, "");                                       \
		return Result(false,                                                       \
			std::string("Expected: (") + ta + ") " opText " (" + tb                \
			+ "), actual: " + printValue(a) + " vs " + printValue(b));             \
	}

RETDEC_GTL_REL_CMP(cmpLt, <, "<")
RETDEC_GTL_REL_CMP(cmpLe, <=, "<=")
RETDEC_GTL_REL_CMP(cmpGt, >, ">")
RETDEC_GTL_REL_CMP(cmpGe, >=, ">=")
#undef RETDEC_GTL_REL_CMP

inline Result cmpBool(bool actual, bool expected, const char* text)
{
	if (actual == expected) return Result(true, "");
	return Result(false,
		std::string("Value of: ") + text
		+ "\n  Actual: " + (actual ? "true" : "false")
		+ "\nExpected: " + (expected ? "true" : "false"));
}

inline Result cmpStr(const char* a, const char* b, bool wantEqual, const char* ta, const char* tb)
{
	const bool equal = (a == nullptr || b == nullptr) ? (a == b) : (std::strcmp(a, b) == 0);
	if (equal == wantEqual) return Result(true, "");
	return Result(false,
		std::string("Expected: (") + ta + ") " + (wantEqual ? "==" : "!=") + " (" + tb
		+ "), actual: " + printValue(a) + " vs " + printValue(b));
}

inline Result cmpNear(double a, double b, double tol, const char* ta, const char* tb, const char* tt)
{
	const double diff = std::fabs(a - b);
	if (!(diff > tol)) return Result(true, "");
	std::ostringstream os;
	os << "The difference between " << ta << " and " << tb << " is " << diff
	   << ", which exceeds " << tt << ", where\n    " << ta << " evaluates to " << a
	   << ",\n    " << tb << " evaluates to " << b
	   << ",\n    " << tt << " evaluates to " << tol << ".";
	return Result(false, os.str());
}

/// GoogleTest's *_FLOAT_EQ / *_DOUBLE_EQ compare within 4 units in the last
/// place.  Reproduce that rather than an arbitrary epsilon, so moving a test
/// between the two harnesses does not silently change what it asserts.
template <typename F>
Result cmpUlp(F a, F b, const char* ta, const char* tb)
{
	if (std::isnan(a) || std::isnan(b))
		return Result(false, std::string("Expected: ") + ta + " == " + tb + ", but a value is NaN");
	if (a == b) return Result(true, "");
	if (std::isinf(a) || std::isinf(b))
		return Result(false, std::string("Expected: ") + ta + " == " + tb + ", but a value is infinite");

	using Bits = std::conditional_t<sizeof(F) == 4, std::uint32_t, std::uint64_t>;
	static_assert(sizeof(Bits) == sizeof(F), "unexpected floating-point width");
	Bits ba = 0, bb = 0;
	std::memcpy(&ba, &a, sizeof(F));
	std::memcpy(&bb, &b, sizeof(F));
	const Bits signBit = Bits(1) << (sizeof(Bits) * 8 - 1);
	// Map to a biased ordering in which adjacent representable values differ by 1.
	const auto biased = [signBit](Bits x) {
		return (x & signBit) ? static_cast<Bits>(signBit - (x & ~signBit))
							 : static_cast<Bits>(signBit + x);
	};
	const Bits xa = biased(ba), xb = biased(bb);
	const Bits ulps = xa >= xb ? static_cast<Bits>(xa - xb) : static_cast<Bits>(xb - xa);
	if (ulps <= 4) return Result(true, "");

	std::ostringstream os;
	os << "Expected equality of these values:\n    " << ta << "\n      Which is: " << a
	   << "\n    " << tb << "\n      Which is: " << b
	   << "\n  (" << ulps << " ULPs apart, limit is 4)";
	return Result(false, os.str());
}

// ─── Test registry ───────────────────────────────────────────────────────────

struct TestCase {
	std::string suite;
	std::string name;
	std::function<void()> body;
};

inline std::vector<TestCase>& registry()
{
	static std::vector<TestCase> r;
	return r;
}

struct Registrar {
	Registrar(std::string suite, std::string name, std::function<void()> body)
	{
		registry().push_back(TestCase{std::move(suite), std::move(name), std::move(body)});
	}
};

/// TEST_P bodies are recorded per fixture type; INSTANTIATE_TEST_SUITE_P then
/// expands them across the generator's values.  Both happen during static
/// initialisation, and within a translation unit that runs top to bottom — the
/// same ordering constraint real GoogleTest has.
template <typename Fixture>
struct ParamBody {
	std::string name;
	void (*fn)(const typename Fixture::ParamType&);
};

template <typename Fixture>
std::vector<ParamBody<Fixture>>& paramBodies()
{
	static std::vector<ParamBody<Fixture>> v;
	return v;
}

/// `--gtest_filter` accepts `pos1:pos2:-neg1:neg2`; each pattern may use `*`/`?`.
inline bool globMatch(const char* pat, const char* str)
{
	if (*pat == '\0') return *str == '\0';
	if (*pat == '*') return globMatch(pat + 1, str) || (*str != '\0' && globMatch(pat, str + 1));
	if (*str == '\0') return false;
	if (*pat == '?' || *pat == *str) return globMatch(pat + 1, str + 1);
	return false;
}

inline bool matchesFilter(const std::string& filter, const std::string& full)
{
	if (filter.empty() || filter == "*") return true;

	std::string positive = filter, negative;
	const auto dash = filter.find('-');
	if (dash != std::string::npos)
	{
		positive = filter.substr(0, dash);
		negative = filter.substr(dash + 1);
	}

	const auto anyMatch = [&full](const std::string& list) {
		std::string item;
		std::istringstream is(list);
		while (std::getline(is, item, ':'))
			if (!item.empty() && globMatch(item.c_str(), full.c_str())) return true;
		return false;
	};

	if (!positive.empty() && !anyMatch(positive)) return false;
	if (!negative.empty() && anyMatch(negative)) return false;
	return true;
}

struct Options {
	std::string filter = "*";
	bool listTests = false;
	bool brief = false;
	int repeat = 1;
};

inline Options& options()
{
	static Options o;
	return o;
}

}  // namespace lite

// ─── Public ::testing surface ────────────────────────────────────────────────

class Test {
public:
	virtual ~Test() = default;
	static void SetUpTestSuite() {}
	static void TearDownTestSuite() {}

	/// Runs SetUp / body / TearDown.  Public so the generated per-test subclass
	/// can drive it from a static function.
	void gtlRun(const std::function<void()>& body)
	{
		SetUp();
		body();
		TearDown();
	}

protected:
	virtual void SetUp() {}
	virtual void TearDown() {}
};

template <typename T>
class TestWithParam : public Test {
public:
	using ParamType = T;

	static const T*& gtlCurrentParam()
	{
		static const T* p = nullptr;
		return p;
	}

	const T& GetParam() const { return *gtlCurrentParam(); }
};

template <typename T>
std::vector<T> ValuesIn(const std::vector<T>& v) { return v; }

template <typename T, std::size_t N>
std::vector<T> ValuesIn(const T (&arr)[N]) { return std::vector<T>(arr, arr + N); }

template <typename It>
auto ValuesIn(It first, It last)
	-> std::vector<typename std::iterator_traits<It>::value_type>
{
	return std::vector<typename std::iterator_traits<It>::value_type>(first, last);
}

template <typename T, typename... Rest>
std::vector<T> Values(T first, Rest... rest)
{
	return std::vector<T>{first, static_cast<T>(rest)...};
}

void InitGoogleTest(int* argc, char** argv);
void InitGoogleTest();

}  // namespace testing

int RUN_ALL_TESTS();

// ─── Macros ──────────────────────────────────────────────────────────────────

#define GTEST_LITE_CONCAT_(a, b) GTEST_LITE_CONCAT_INNER_(a, b)
#define GTEST_LITE_CONCAT_INNER_(a, b) a##b

#define GTEST_LITE_MESSAGE_ ::testing::lite::Message()

#define GTEST_LITE_FAIL_(result, fatal) \
	::testing::lite::AssertHelper((result), (fatal), __FILE__, __LINE__) = GTEST_LITE_MESSAGE_

// `switch (0) case 0: default:` keeps this a single statement, so a caller's
// dangling `else` still binds correctly.
#define GTEST_LITE_ASSERT_(expr, on_failure)                    \
	switch (0)                                                  \
	case 0:                                                     \
	default:                                                    \
		if (const ::testing::lite::Result gtl_res_ = (expr)) {}  \
		else                                                    \
			on_failure(gtl_res_)

#define GTEST_LITE_NONFATAL_(result) GTEST_LITE_FAIL_(result, false)
#define GTEST_LITE_FATAL_(result) return GTEST_LITE_FAIL_(result, true)

#define EXPECT_TRUE(c)  GTEST_LITE_ASSERT_(::testing::lite::cmpBool(static_cast<bool>(c), true,  #c), GTEST_LITE_NONFATAL_)
#define EXPECT_FALSE(c) GTEST_LITE_ASSERT_(::testing::lite::cmpBool(static_cast<bool>(c), false, #c), GTEST_LITE_NONFATAL_)
#define ASSERT_TRUE(c)  GTEST_LITE_ASSERT_(::testing::lite::cmpBool(static_cast<bool>(c), true,  #c), GTEST_LITE_FATAL_)
#define ASSERT_FALSE(c) GTEST_LITE_ASSERT_(::testing::lite::cmpBool(static_cast<bool>(c), false, #c), GTEST_LITE_FATAL_)

#define EXPECT_EQ(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpEq((a), (b), #a, #b), GTEST_LITE_NONFATAL_)
#define EXPECT_NE(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpNe((a), (b), #a, #b), GTEST_LITE_NONFATAL_)
#define EXPECT_LT(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpLt((a), (b), #a, #b), GTEST_LITE_NONFATAL_)
#define EXPECT_LE(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpLe((a), (b), #a, #b), GTEST_LITE_NONFATAL_)
#define EXPECT_GT(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpGt((a), (b), #a, #b), GTEST_LITE_NONFATAL_)
#define EXPECT_GE(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpGe((a), (b), #a, #b), GTEST_LITE_NONFATAL_)

#define ASSERT_EQ(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpEq((a), (b), #a, #b), GTEST_LITE_FATAL_)
#define ASSERT_NE(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpNe((a), (b), #a, #b), GTEST_LITE_FATAL_)
#define ASSERT_LT(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpLt((a), (b), #a, #b), GTEST_LITE_FATAL_)
#define ASSERT_LE(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpLe((a), (b), #a, #b), GTEST_LITE_FATAL_)
#define ASSERT_GT(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpGt((a), (b), #a, #b), GTEST_LITE_FATAL_)
#define ASSERT_GE(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpGe((a), (b), #a, #b), GTEST_LITE_FATAL_)

#define EXPECT_STREQ(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpStr((a), (b), true,  #a, #b), GTEST_LITE_NONFATAL_)
#define EXPECT_STRNE(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpStr((a), (b), false, #a, #b), GTEST_LITE_NONFATAL_)
#define ASSERT_STREQ(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpStr((a), (b), true,  #a, #b), GTEST_LITE_FATAL_)
#define ASSERT_STRNE(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpStr((a), (b), false, #a, #b), GTEST_LITE_FATAL_)

#define EXPECT_NEAR(a, b, t) GTEST_LITE_ASSERT_(::testing::lite::cmpNear((a), (b), (t), #a, #b, #t), GTEST_LITE_NONFATAL_)
#define ASSERT_NEAR(a, b, t) GTEST_LITE_ASSERT_(::testing::lite::cmpNear((a), (b), (t), #a, #b, #t), GTEST_LITE_FATAL_)

#define EXPECT_DOUBLE_EQ(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpUlp<double>((a), (b), #a, #b), GTEST_LITE_NONFATAL_)
#define ASSERT_DOUBLE_EQ(a, b) GTEST_LITE_ASSERT_(::testing::lite::cmpUlp<double>((a), (b), #a, #b), GTEST_LITE_FATAL_)
#define EXPECT_FLOAT_EQ(a, b)  GTEST_LITE_ASSERT_(::testing::lite::cmpUlp<float>((a), (b), #a, #b), GTEST_LITE_NONFATAL_)
#define ASSERT_FLOAT_EQ(a, b)  GTEST_LITE_ASSERT_(::testing::lite::cmpUlp<float>((a), (b), #a, #b), GTEST_LITE_FATAL_)

#define GTEST_LITE_NO_THROW_(stmt, on_failure)                                              \
	GTEST_LITE_ASSERT_(                                                                     \
		[&]() -> ::testing::lite::Result {                                                  \
			try { stmt; }                                                                   \
			catch (const std::exception& e) {                                               \
				return ::testing::lite::Result(false,                                       \
					std::string("Expected: " #stmt " does not throw.\n  Actual: it throws: ")\
					+ e.what());                                                            \
			}                                                                               \
			catch (...) {                                                                   \
				return ::testing::lite::Result(false,                                       \
					"Expected: " #stmt " does not throw.\n  Actual: it throws.");            \
			}                                                                               \
			return ::testing::lite::Result(true, "");                                       \
		}(),                                                                                \
		on_failure)

#define EXPECT_NO_THROW(stmt) GTEST_LITE_NO_THROW_(stmt, GTEST_LITE_NONFATAL_)
#define ASSERT_NO_THROW(stmt) GTEST_LITE_NO_THROW_(stmt, GTEST_LITE_FATAL_)

#define GTEST_LITE_THROW_(stmt, ex, on_failure)                                             \
	GTEST_LITE_ASSERT_(                                                                     \
		[&]() -> ::testing::lite::Result {                                                  \
			try { stmt; }                                                                   \
			catch (const ex&) { return ::testing::lite::Result(true, ""); }                 \
			catch (...) {                                                                   \
				return ::testing::lite::Result(false,                                       \
					"Expected: " #stmt " throws " #ex ".\n  Actual: it throws a "           \
					"different type.");                                                     \
			}                                                                               \
			return ::testing::lite::Result(false,                                           \
				"Expected: " #stmt " throws " #ex ".\n  Actual: it throws nothing.");        \
		}(),                                                                                \
		on_failure)

#define GTEST_LITE_ANY_THROW_(stmt, on_failure)                                             \
	GTEST_LITE_ASSERT_(                                                                     \
		[&]() -> ::testing::lite::Result {                                                  \
			try { stmt; }                                                                   \
			catch (...) { return ::testing::lite::Result(true, ""); }                       \
			return ::testing::lite::Result(false,                                           \
				"Expected: " #stmt " throws an exception.\n  Actual: it throws nothing.");    \
		}(),                                                                                \
		on_failure)

#define EXPECT_THROW(stmt, ex) GTEST_LITE_THROW_(stmt, ex, GTEST_LITE_NONFATAL_)
#define ASSERT_THROW(stmt, ex) GTEST_LITE_THROW_(stmt, ex, GTEST_LITE_FATAL_)
#define EXPECT_ANY_THROW(stmt) GTEST_LITE_ANY_THROW_(stmt, GTEST_LITE_NONFATAL_)
#define ASSERT_ANY_THROW(stmt) GTEST_LITE_ANY_THROW_(stmt, GTEST_LITE_FATAL_)

#define ADD_FAILURE() GTEST_LITE_FAIL_(::testing::lite::Result(false, "Failed"), false)
#define FAIL() return GTEST_LITE_FAIL_(::testing::lite::Result(false, "Failed"), true)
#define SUCCEED() (void) 0
#define GTEST_SKIP() return ::testing::lite::SkipHelper(__FILE__, __LINE__) = GTEST_LITE_MESSAGE_

#define GTEST_LITE_CLASS_(suite, name) suite##_##name##_GtlTest

/// A plain test: the body is a free function.
#define TEST(suite, name)                                                                   \
	static void GTEST_LITE_CLASS_(suite, name)();                                           \
	static const ::testing::lite::Registrar GTEST_LITE_CONCAT_(gtlReg_, __LINE__)(          \
		#suite, #name, &GTEST_LITE_CLASS_(suite, name));                                    \
	static void GTEST_LITE_CLASS_(suite, name)()

/// A fixture test: the body becomes a method of a subclass of the fixture, so it
/// reaches protected members exactly as it does under real GoogleTest.
#define TEST_F(fixture, name)                                                               \
	namespace {                                                                             \
	class GTEST_LITE_CLASS_(fixture, name): public fixture {                                \
	public:                                                                                 \
		void gtlBody();                                                                     \
		static void gtlEntry()                                                              \
		{                                                                                   \
			GTEST_LITE_CLASS_(fixture, name) t;                                             \
			t.gtlRun([&t]() { t.gtlBody(); });                                              \
		}                                                                                   \
	};                                                                                      \
	}                                                                                       \
	static const ::testing::lite::Registrar GTEST_LITE_CONCAT_(gtlReg_, __LINE__)(          \
		#fixture, #name, &GTEST_LITE_CLASS_(fixture, name)::gtlEntry);                      \
	void GTEST_LITE_CLASS_(fixture, name)::gtlBody()

/// A parameterised test.  The body compiles once; INSTANTIATE_TEST_SUITE_P
/// registers one registry entry per parameter value.
#define TEST_P(fixture, name)                                                               \
	namespace {                                                                             \
	class GTEST_LITE_CLASS_(fixture, name): public fixture {                                \
	public:                                                                                 \
		void gtlBody();                                                                     \
		static void gtlEntry(const fixture::ParamType& p)                                   \
		{                                                                                   \
			GTEST_LITE_CLASS_(fixture, name) t;                                             \
			fixture::gtlCurrentParam() = &p;                                                \
			t.gtlRun([&t]() { t.gtlBody(); });                                               \
			fixture::gtlCurrentParam() = nullptr;                                           \
		}                                                                                   \
	};                                                                                      \
	}                                                                                       \
	static const int GTEST_LITE_CONCAT_(gtlParamReg_, __LINE__) = []() {                    \
		::testing::lite::paramBodies<fixture>().push_back(                                  \
			{#name, &GTEST_LITE_CLASS_(fixture, name)::gtlEntry});                          \
		return 0;                                                                           \
	}();                                                                                    \
	void GTEST_LITE_CLASS_(fixture, name)::gtlBody()

#define INSTANTIATE_TEST_SUITE_P(prefix, fixture, generator, ...)                           \
	static const int GTEST_LITE_CONCAT_(gtlInst_, __LINE__) = []() {                        \
		/* `static` so the parameter objects outlive registration. */                       \
		static const auto gtlParams = (generator);                                          \
		int index = 0;                                                                      \
		for (const auto& p: gtlParams)                                                      \
		{                                                                                   \
			const auto* pp = &p;                                                            \
			const int i = index++;                                                          \
			for (const auto& body: ::testing::lite::paramBodies<fixture>())                 \
			{                                                                               \
				const auto fn = body.fn;                                                    \
				::testing::lite::registry().push_back(::testing::lite::TestCase{            \
					std::string(#prefix "/" #fixture),                                      \
					body.name + "/" + std::to_string(i),                                    \
					[fn, pp]() { fn(*pp); }});                                               \
			}                                                                               \
		}                                                                                   \
		return 0;                                                                           \
	}()

#endif  // RETDEC_TESTS_STANDALONE_GTEST_LITE_H
