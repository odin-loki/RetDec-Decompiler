/**
 * @file tests/standalone/gtest_lite.cpp
 * @brief Runtime for the dependency-free GoogleTest shim.
 *
 * See tests/standalone/gtest/gtest.h and scripts/standalone_check.sh.
 */

#include "gtest/gtest.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace {

bool startsWith(const char* s, const char* prefix)
{
	return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

const char* green(bool colour) { return colour ? "\033[0;32m" : ""; }
const char* red(bool colour) { return colour ? "\033[0;31m" : ""; }
const char* yellow(bool colour) { return colour ? "\033[0;33m" : ""; }
const char* reset(bool colour) { return colour ? "\033[m" : ""; }

bool useColour()
{
	const char* term = std::getenv("TERM");
	if (std::getenv("GTEST_LITE_NO_COLOR") != nullptr) return false;
	return term != nullptr && std::strcmp(term, "dumb") != 0;
}

}  // namespace

namespace testing {

void InitGoogleTest(int* argc, char** argv)
{
	auto& opts = lite::options();
	int out = 1;
	for (int i = 1; i < *argc; ++i)
	{
		const char* a = argv[i];
		if (startsWith(a, "--gtest_filter="))
		{
			opts.filter = a + std::strlen("--gtest_filter=");
		}
		else if (std::strcmp(a, "--gtest_list_tests") == 0)
		{
			opts.listTests = true;
		}
		else if (std::strcmp(a, "--gtest_brief") == 0 || std::strcmp(a, "--gtest_brief=1") == 0)
		{
			opts.brief = true;
		}
		else if (startsWith(a, "--gtest_repeat="))
		{
			opts.repeat = std::max(1, std::atoi(a + std::strlen("--gtest_repeat=")));
		}
		else if (startsWith(a, "--gtest_"))
		{
			// Accept and ignore the remaining GoogleTest flags (--gtest_color,
			// --gtest_output, --gtest_shuffle, ...) so existing CI invocations
			// keep working against this shim.
		}
		else
		{
			argv[out++] = argv[i];
		}
	}
	*argc = out;
	argv[out] = nullptr;
}

void InitGoogleTest()
{
	int argc = 1;
	char program[] = "gtest-lite";
	char* argv[] = {program, nullptr};
	char** p = argv;
	InitGoogleTest(&argc, p);
}

}  // namespace testing

int RUN_ALL_TESTS()
{
	using namespace testing::lite;

	const auto& opts = options();
	const bool colour = useColour();
	auto& all = registry();

	std::vector<const TestCase*> selected;
	selected.reserve(all.size());
	for (const auto& t: all)
	{
		if (matchesFilter(opts.filter, t.suite + "." + t.name)) selected.push_back(&t);
	}

	if (opts.listTests)
	{
		std::string lastSuite;
		for (const auto* t: selected)
		{
			if (t->suite != lastSuite)
			{
				std::cout << t->suite << ".\n";
				lastSuite = t->suite;
			}
			std::cout << "  " << t->name << "\n";
		}
		return 0;
	}

	std::cout << "[==========] Running " << selected.size() << " test"
			  << (selected.size() == 1 ? "" : "s") << " (gtest-lite, "
			  << all.size() << " registered).\n";

	int failed = 0;
	int skipped = 0;
	std::vector<std::string> failedNames;
	const auto begin = std::chrono::steady_clock::now();

	for (int rep = 0; rep < opts.repeat; ++rep)
	{
		for (const auto* t: selected)
		{
			const std::string full = t->suite + "." + t->name;
			state() = State{};

			if (!opts.brief) std::cout << "[ RUN      ] " << full << "\n";

			try
			{
				t->body();
			}
			catch (const std::exception& e)
			{
				state().failed = true;
				state().failures.push_back(
					std::string("uncaught exception: ") + e.what());
			}
			catch (...)
			{
				state().failed = true;
				state().failures.push_back("uncaught non-standard exception");
			}

			if (state().failed)
			{
				++failed;
				failedNames.push_back(full);
				for (const auto& f: state().failures)
					std::cout << f << "\n";
				std::cout << red(colour) << "[  FAILED  ] " << reset(colour) << full << "\n";
			}
			else if (state().skipped)
			{
				++skipped;
				if (!opts.brief)
					std::cout << yellow(colour) << "[  SKIPPED ] " << reset(colour)
							  << full << " (" << state().skipReason << ")\n";
			}
			else if (!opts.brief)
			{
				std::cout << green(colour) << "[       OK ] " << reset(colour) << full << "\n";
			}
		}
	}

	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now() - begin)
						.count();

	const std::size_t ran = selected.size() * static_cast<std::size_t>(opts.repeat);
	std::cout << "[==========] " << ran << " test" << (ran == 1 ? "" : "s")
			  << " ran (" << ms << " ms total).\n";
	std::cout << green(colour) << "[  PASSED  ] " << reset(colour)
			  << (ran - static_cast<std::size_t>(failed) - static_cast<std::size_t>(skipped))
			  << " test(s).\n";
	if (skipped > 0)
		std::cout << yellow(colour) << "[  SKIPPED ] " << reset(colour) << skipped << " test(s).\n";
	if (failed > 0)
	{
		std::cout << red(colour) << "[  FAILED  ] " << reset(colour) << failed << " test(s):\n";
		for (const auto& n: failedNames) std::cout << red(colour) << "[  FAILED  ] " << reset(colour) << n << "\n";
	}

	return failed == 0 ? 0 : 1;
}
