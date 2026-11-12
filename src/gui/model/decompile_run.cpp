/**
 * @file src/gui/model/decompile_run.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/decompile_run.h"

#include <forge/os/fs.hpp>
#include <forge/os/process.hpp>

#include <condition_variable>
#include <format>
#include <mutex>
#include <system_error>
#include <thread>

namespace retdec::gui::model {

namespace fs = std::filesystem;

struct DecompileRun::Worker
{
	std::mutex mutex;
	std::condition_variable cv;
	bool done = false;
	std::chrono::steady_clock::time_point ended{};
	forge::os::result<forge::os::proc_result> result{};
	std::jthread thread; // last: joined (after a stop request) before the rest is destroyed
};

DecompileRun::DecompileRun() = default;

DecompileRun::~DecompileRun()
{
	if (worker_)
	{
		worker_->thread.request_stop();
		if (worker_->thread.joinable()) worker_->thread.join();
		worker_.reset();
	}
	removeOwnedLog();
}

void DecompileRun::removeOwnedLog()
{
	if (ownsLog_ && !logPath_.empty())
	{
		std::error_code ec;
		fs::remove(logPath_, ec);
	}
	ownsLog_ = false;
}

std::expected<void, std::string> DecompileRun::start(DecompileRunSpec spec)
{
	if (state_ == State::Running) return std::unexpected(std::string("A decompiler run is already in progress."));
	if (worker_)
	{
		if (worker_->thread.joinable()) worker_->thread.join();
		worker_.reset();
	}
	removeOwnedLog();
	logPath_.clear();
	state_ = State::NotStarted;
	exitCode_ = -1;
	termSignal_ = 0;
	timedOut_ = false;
	peakRss_ = 0;
	error_.clear();

	if (spec.executable.empty()) return std::unexpected(std::string("No executable to run."));
	const std::string exe = forge::os::find_executable(spec.executable.string());
	if (exe.empty()) return std::unexpected(std::format("Failed to start {}: not found or not executable.", spec.executable.string()));
	if (spec.workingDirectory.empty()) spec.workingDirectory = fs::path(exe).parent_path();
	{
		std::error_code ec;
		if (!spec.workingDirectory.empty() && !fs::is_directory(spec.workingDirectory, ec))
			return std::unexpected(std::format("Working directory does not exist: {}", spec.workingDirectory.string()));
	}

	if (spec.logPath.empty())
	{
		auto tmp = forge::os::fs::make_temp_file("retdec-decompile-", ".log");
		if (!tmp) return std::unexpected(std::string("Could not create a temporary log file for decompiler output."));
		logPath_ = fs::path(tmp.value);
		ownsLog_ = true;
	}
	else
	{
		logPath_ = spec.logPath;
		ownsLog_ = false;
	}

	forge::os::spawn_spec sp;
	sp.argv.reserve(spec.arguments.size() + 1);
	sp.argv.push_back(exe);
	for (const std::string& a: spec.arguments)
		sp.argv.push_back(a);
	sp.cwd = spec.workingDirectory.string();
	sp.env_set = spec.environment;
	sp.stdout_file = logPath_.string();
	sp.merge_stderr = true;
	sp.capture_stdout = false;
	sp.capture_stderr = false;
	sp.timeout_ms = spec.timeoutMs;

	spec_ = std::move(spec);
	spec_.executable = fs::path(exe);
	worker_ = std::make_unique<Worker>();
	started_ = std::chrono::steady_clock::now();
	ended_ = {};
	state_ = State::Running;

	Worker* w = worker_.get();
	w->thread = std::jthread([w, sp = std::move(sp)](std::stop_token stop) mutable {
		sp.stop = stop;
		auto r = forge::os::spawn_wait(sp);
		{
			std::lock_guard lock(w->mutex);
			w->result = std::move(r);
			w->ended = std::chrono::steady_clock::now();
			w->done = true;
		}
		w->cv.notify_all();
	});
	return {};
}

void DecompileRun::collect()
{
	// Called once the worker reported done.
	if (worker_->thread.joinable()) worker_->thread.join();
	const auto& r = worker_->result;
	ended_ = worker_->ended;
	if (!r)
	{
		state_ = State::FailedToStart;
		error_ = r.err.what.empty() ? std::string("Failed to start retdec-decompiler.") : r.err.what;
		exitCode_ = -1;
	}
	else if (r.value.cancelled)
	{
		state_ = State::Cancelled;
		exitCode_ = r.value.exit_code;
	}
	else
	{
		state_ = State::Finished;
		exitCode_ = r.value.exit_code;
		termSignal_ = r.value.term_signal;
		timedOut_ = r.value.timed_out;
	}
	if (r) peakRss_ = r.value.peak_rss_bytes;
	worker_.reset();
}

bool DecompileRun::poll()
{
	if (state_ != State::Running) return state_ != State::NotStarted;
	{
		std::lock_guard lock(worker_->mutex);
		if (!worker_->done) return false;
	}
	collect();
	return true;
}

bool DecompileRun::waitFor(std::chrono::milliseconds timeout)
{
	if (state_ != State::Running) return state_ != State::NotStarted;
	{
		std::unique_lock lock(worker_->mutex);
		if (!worker_->cv.wait_for(lock, timeout, [this] { return worker_->done; })) return false;
	}
	collect();
	return true;
}

void DecompileRun::cancel()
{
	if (state_ != State::Running) return;
	worker_->thread.request_stop();
	{
		// spawn_wait notices the stop request within ~50 ms, kills the tree and
		// reaps it (giving up on an unkillable child after 2 s).
		std::unique_lock lock(worker_->mutex);
		worker_->cv.wait(lock, [this] { return worker_->done; });
	}
	collect();
	if (state_ == State::Finished) state_ = State::Cancelled; // exited on its own just as we cancelled
}

std::optional<int> DecompileRun::exitCode() const
{
	if (state_ == State::Finished || state_ == State::Cancelled) return exitCode_;
	return std::nullopt;
}

std::chrono::milliseconds DecompileRun::elapsed() const
{
	if (state_ == State::NotStarted) return std::chrono::milliseconds{0};
	const auto end = state_ == State::Running ? std::chrono::steady_clock::now() : ended_;
	return std::chrono::duration_cast<std::chrono::milliseconds>(end - started_);
}

} // namespace retdec::gui::model
