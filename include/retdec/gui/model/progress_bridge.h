/**
 * @file include/retdec/gui/model/progress_bridge.h
 * @brief Thread-safe bridge from the analysis pipeline (any thread) to
 *        whoever displays progress (port of the Qt AnalysisBridge).
 *
 * Pipeline code calls report*(); every subscribed Listener's matching
 * callback runs on the reporting thread, under the bridge's lock, so a
 * subscription that is destroyed is never called afterwards. Listeners must be
 * quick and must not call back into the bridge: the ProgressPanel's listener
 * only queues the event and posts it to the UI thread (UiContext::post).
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_PROGRESS_BRIDGE_H
#define RETDEC_GUI_MODEL_PROGRESS_BRIDGE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace retdec::gui::model {

class AnalysisBridge
{
public:
	/// Unset members are skipped.
	struct Listener
	{
		std::function<void(const std::string& stage)> stageStarted;
		std::function<void(const std::string& stage, int percent)> progressUpdated;
		std::function<void(const std::string& stage, int64_t elapsedMs)> stageCompleted;
		std::function<void(const std::string& stage, const std::string& message)> stageErrored;
		std::function<void(int count)> functionCountChanged;
		std::function<void(int64_t count)> instructionCountChanged;
		std::function<void(double fnPerSec, double instrPerSec)> throughputChanged;
		std::function<void()> analysisStarted;
		std::function<void(int64_t totalMs)> analysisCompleted;
		std::function<void()> analysisCancelled;
	};

	/// Keeps a listener subscribed while alive (move-only). Outliving the
	/// bridge is fine.
	class Subscription
	{
	public:
		Subscription() = default;
		Subscription(Subscription&& o) noexcept;
		Subscription& operator=(Subscription&& o) noexcept;
		Subscription(const Subscription&) = delete;
		Subscription& operator=(const Subscription&) = delete;
		~Subscription();
		void reset();
		bool active() const;

	private:
		friend class AnalysisBridge;
		Subscription(std::weak_ptr<void> state, uint64_t id): state_(std::move(state)), id_(id) {}
		std::weak_ptr<void> state_;
		uint64_t id_ = 0;
	};

	AnalysisBridge();
	~AnalysisBridge();
	AnalysisBridge(const AnalysisBridge&) = delete;
	AnalysisBridge& operator=(const AnalysisBridge&) = delete;

	[[nodiscard]] Subscription subscribe(Listener listener);
	std::size_t listenerCount() const;

	// ── pipeline-facing API (any thread) ────────────────────────────────────
	void reportStageStarted(const std::string& stageName);
	void reportProgress(const std::string& stageName, int percent);
	void reportStageCompleted(const std::string& stageName, int64_t elapsedMs);
	void reportStageError(const std::string& stageName, const std::string& msg);
	void reportFunctionCount(int count);
	void reportInstructionCount(int64_t count);
	void reportThroughput(double fnPerSec, double instrPerSec);
	void reportAnalysisStarted();
	void reportAnalysisCompleted(int64_t totalMs);
	void reportAnalysisCancelled();

private:
	struct Impl;
	template <class F>
	void each(F&& f);
	std::shared_ptr<Impl> impl_;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_PROGRESS_BRIDGE_H
