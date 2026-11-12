/**
 * @file include/retdec/gui/model/decompile_run.h
 * @brief One retdec-decompiler run: spawn, poll, cancel, exit status, log file.
 *
 * Replaces the Qt main window's QProcess handling. The child's stdout and
 * stderr go to a log file, not a pipe: the GUI reads the log at its own pace
 * (pollDecompileLogProgress(), readDecompilerLogIncremental(), ...) and a slow
 * console can never stall the decompiler on a full pipe buffer.
 *
 * The child is started from a forge::os::spawn_spec (argv, cwd, env_set,
 * stdout_file + merge_stderr) and waited for by forge::os::spawn_wait() on a
 * worker thread (forge::os::child only offers a pipe for stdout, not a file).
 * The child leads its own process group / Job Object, so cancel() and the
 * destructor kill the whole tree.
 *
 * Use from one thread (the UI thread): start(), then call poll() from a timer
 * until it returns true, then read state() / exitCode() / elapsed().
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_DECOMPILE_RUN_H
#define RETDEC_GUI_MODEL_DECOMPILE_RUN_H

#include "retdec/gui/model/decompiler_launch.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retdec::gui::model {

struct DecompileRunSpec
{
	/// The program (an absolute path, or a name looked up on PATH).
	std::filesystem::path executable;
	/// argv after the program (buildDecompilerArguments().args).
	std::vector<std::string> arguments;
	/// Empty = the executable's directory (what the Qt GUI used).
	std::filesystem::path workingDirectory;
	/// Variables set on top of the GUI's environment (buildDecompilerProcessEnvironment()).
	ProcessEnvironment environment;
	/// Where stdout+stderr go (created / truncated). Empty = a private temp
	/// file, deleted when the DecompileRun is destroyed or restarted.
	std::filesystem::path logPath;
	/// Kill the tree after this long (0 = no limit). Ends as Finished with exit code 124 and timedOut().
	std::uint64_t timeoutMs = 0;
};

class DecompileRun {
public:
	enum class State
	{
		NotStarted,
		Running,
		Finished,      ///< the process exited (any code, including killed by a signal)
		FailedToStart, ///< spawn failed (not found, not executable, bad cwd, ...): errorString()
		Cancelled,     ///< cancel() killed it
	};

	DecompileRun();
	/// Kills a still-running tree and waits for it; removes a private temp log.
	~DecompileRun();
	DecompileRun(const DecompileRun&) = delete;
	DecompileRun& operator=(const DecompileRun&) = delete;

	/**
	 * @brief Start the process. Fails (and stays not running) when a run is
	 *        already in progress, the executable cannot be found, the working
	 *        directory does not exist or the log file cannot be created. A
	 *        failure the OS only reports after fork/exec shows up as
	 *        State::FailedToStart from poll().
	 */
	std::expected<void, std::string> start(DecompileRunSpec spec);

	/// Non-blocking: true once the run has ended (state() is no longer Running).
	bool poll();
	/// Block up to @p timeout for the run to end; true if it has.
	bool waitFor(std::chrono::milliseconds timeout);
	/// Kill the whole process tree and wait (briefly) for it; state() becomes
	/// Cancelled. No-op when not running.
	void cancel();

	State state() const { return state_; }
	bool running() const { return state_ == State::Running; }
	/// The exit code once Finished (128+signal when killed by one; 124 on timeout), else nothing.
	std::optional<int> exitCode() const;
	/// Signal that ended the process (POSIX), 0 otherwise.
	int termSignal() const { return termSignal_; }
	bool timedOut() const { return timedOut_; }
	/// Why the run failed to start (FailedToStart), else empty.
	const std::string& errorString() const { return error_; }
	/// Wall time since start(), frozen when the run ends.
	std::chrono::milliseconds elapsed() const;
	/// Peak resident memory of the child in bytes once ended (0 = unknown).
	std::uint64_t peakRssBytes() const { return peakRss_; }

	const std::filesystem::path& logPath() const { return logPath_; }
	const std::filesystem::path& executable() const { return spec_.executable; }
	const std::vector<std::string>& arguments() const { return spec_.arguments; }
	const std::filesystem::path& workingDirectory() const { return spec_.workingDirectory; }

private:
	struct Worker;

	void collect();
	void removeOwnedLog();

	DecompileRunSpec spec_;
	std::filesystem::path logPath_;
	bool ownsLog_ = false;
	State state_ = State::NotStarted;
	int exitCode_ = -1;
	int termSignal_ = 0;
	bool timedOut_ = false;
	std::uint64_t peakRss_ = 0;
	std::string error_;
	std::chrono::steady_clock::time_point started_{};
	std::chrono::steady_clock::time_point ended_{};
	std::unique_ptr<Worker> worker_;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_DECOMPILE_RUN_H
