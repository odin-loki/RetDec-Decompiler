/**
 * @file tests/retdec/thread_pool_test.cpp
 * @brief Smoke tests for retdec::utils::ThreadPool.
 */

#include "retdec/utils/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

using retdec::utils::ThreadPool;

TEST(ThreadPoolTest, RunsSubmittedTasks)
{
    ThreadPool pool(2);
    std::atomic<int> counter{0};

    auto f1 = pool.submit([&counter]() { counter.fetch_add(1); });
    auto f2 = pool.submit([&counter]() { counter.fetch_add(10); });
    f1.get();
    f2.get();

    EXPECT_EQ(counter.load(), 11);
}

TEST(ThreadPoolTest, WaitBlocksUntilAllTasksFinish)
{
    ThreadPool pool(2);
    std::atomic<int> counter{0};

    for (int i = 0; i < 8; ++i) {
        pool.submit([&counter]() { counter.fetch_add(1); });
    }
    pool.wait();

    EXPECT_EQ(counter.load(), 8);
}

TEST(ThreadPoolTest, ReturnsValueFromSubmit)
{
    ThreadPool pool(1);
    auto fut = pool.submit([]() { return 42; });
    EXPECT_EQ(fut.get(), 42);
}

// ─── a constructor that cannot start its threads ────────────────────────────
//
// std::thread's constructor throws std::system_error when the process cannot
// start another thread, and every ThreadPool in src/retdec/retdec.cpp is sized
// from hardware_concurrency() or from --jobs.  A constructor that throws does
// not get its destructor run, but its members are destroyed -- and the workers
// that did start are sitting inside workerLoop(), waiting on the very condition
// variable being destroyed.  Measured under `ulimit -v 600000` with 512
// requested threads, that hangs forever:
//
//   #2 __GI___pthread_cond_destroy      nptl/pthread_cond_destroy.c:53
//   #3 retdec::utils::ThreadPool::ThreadPool   thread_pool.h:40
//
// and past that, ~thread on a joinable thread calls std::terminate().
//
// The property is "control comes back": the constructor either builds a pool or
// throws, and never hangs or aborts.  A child process makes both bad outcomes
// observable -- a hang becomes SIGALRM, an abort becomes SIGABRT -- without
// putting either into this suite.

#ifndef _WIN32

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>

namespace {

/// Exit codes the child uses to report what it saw.
enum ChildResult : int
{
	kThrewAndReturned = 0, ///< The interesting case: it failed and unwound.
	kBuiltThePool     = 1, ///< The limit did not bite here; nothing to check.
	kSetrlimitFailed  = 2,
};

} // namespace

TEST(ThreadPoolTest, AConstructorThatCannotStartItsThreadsReturnsControl)
{
	const pid_t pid = fork();
	ASSERT_NE(-1, pid) << "fork failed";

	if (pid == 0)
	{
		// A hang is the failure being tested for, so it has to become a signal.
		alarm(30);

		// Small enough that 4096 thread stacks cannot be mapped, large enough
		// that the process itself keeps running.
		const rlim_t cap = 600u * 1024 * 1024;
		struct rlimit lim
		{
			cap, cap
		};
		if (setrlimit(RLIMIT_AS, &lim) != 0) _exit(kSetrlimitFailed);

		try
		{
			ThreadPool pool(4096);
			_exit(kBuiltThePool);
		}
		catch (const std::exception&)
		{
			_exit(kThrewAndReturned);
		}
		_exit(kBuiltThePool);
	}

	int status = 0;
	ASSERT_EQ(pid, waitpid(pid, &status, 0));

	ASSERT_TRUE(WIFEXITED(status))
		<< "the constructor did not return control: child killed by signal "
		<< (WIFSIGNALED(status) ? WTERMSIG(status) : -1)
		<< " (SIGALRM " << SIGALRM << " is the hang, SIGABRT " << SIGABRT
		<< " is std::terminate)";

	// Either outcome is fine; only a hang or an abort is not.
	EXPECT_NE(kSetrlimitFailed, WEXITSTATUS(status))
		<< "could not lower RLIMIT_AS, so nothing was exercised";
}

#endif // _WIN32
