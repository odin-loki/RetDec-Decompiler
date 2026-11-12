/**
 * @file src/gui/model/progress_bridge.cpp
 * @brief Thread-safe analysis progress bridge.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/progress_bridge.h"

#include <mutex>
#include <utility>
#include <vector>

namespace retdec::gui::model {

struct AnalysisBridge::Impl
{
	std::mutex mutex;
	uint64_t nextId = 1;
	std::vector<std::pair<uint64_t, Listener>> listeners;

	void remove(uint64_t id)
	{
		std::lock_guard lock(mutex);
		std::erase_if(listeners, [id](const auto& p) { return p.first == id; });
	}
};

// ─── Subscription ────────────────────────────────────────────────────────────

AnalysisBridge::Subscription::Subscription(Subscription&& o) noexcept: state_(std::move(o.state_)), id_(o.id_)
{
	o.id_ = 0;
}

AnalysisBridge::Subscription& AnalysisBridge::Subscription::operator=(Subscription&& o) noexcept
{
	if (this != &o)
	{
		reset();
		state_ = std::move(o.state_);
		id_ = o.id_;
		o.id_ = 0;
	}
	return *this;
}

AnalysisBridge::Subscription::~Subscription()
{
	reset();
}

void AnalysisBridge::Subscription::reset()
{
	if (auto s = state_.lock()) static_cast<Impl*>(s.get())->remove(id_);
	state_.reset();
	id_ = 0;
}

bool AnalysisBridge::Subscription::active() const
{
	return id_ != 0 && !state_.expired();
}

// ─── AnalysisBridge ──────────────────────────────────────────────────────────

AnalysisBridge::AnalysisBridge(): impl_(std::make_shared<Impl>()) {}

AnalysisBridge::~AnalysisBridge() = default;

AnalysisBridge::Subscription AnalysisBridge::subscribe(Listener listener)
{
	std::lock_guard lock(impl_->mutex);
	const uint64_t id = impl_->nextId++;
	impl_->listeners.emplace_back(id, std::move(listener));
	return Subscription(std::weak_ptr<void>(std::static_pointer_cast<void>(impl_)), id);
}

std::size_t AnalysisBridge::listenerCount() const
{
	std::lock_guard lock(impl_->mutex);
	return impl_->listeners.size();
}

template <class F>
void AnalysisBridge::each(F&& f)
{
	std::lock_guard lock(impl_->mutex);
	for (auto& [id, l]: impl_->listeners)
		f(l);
}

void AnalysisBridge::reportStageStarted(const std::string& stageName)
{
	each([&](Listener& l) {
		if (l.stageStarted) l.stageStarted(stageName);
	});
}

void AnalysisBridge::reportProgress(const std::string& stageName, int percent)
{
	each([&](Listener& l) {
		if (l.progressUpdated) l.progressUpdated(stageName, percent);
	});
}

void AnalysisBridge::reportStageCompleted(const std::string& stageName, int64_t elapsedMs)
{
	each([&](Listener& l) {
		if (l.stageCompleted) l.stageCompleted(stageName, elapsedMs);
	});
}

void AnalysisBridge::reportStageError(const std::string& stageName, const std::string& msg)
{
	each([&](Listener& l) {
		if (l.stageErrored) l.stageErrored(stageName, msg);
	});
}

void AnalysisBridge::reportFunctionCount(int count)
{
	each([&](Listener& l) {
		if (l.functionCountChanged) l.functionCountChanged(count);
	});
}

void AnalysisBridge::reportInstructionCount(int64_t count)
{
	each([&](Listener& l) {
		if (l.instructionCountChanged) l.instructionCountChanged(count);
	});
}

void AnalysisBridge::reportThroughput(double fnPerSec, double instrPerSec)
{
	each([&](Listener& l) {
		if (l.throughputChanged) l.throughputChanged(fnPerSec, instrPerSec);
	});
}

void AnalysisBridge::reportAnalysisStarted()
{
	each([&](Listener& l) {
		if (l.analysisStarted) l.analysisStarted();
	});
}

void AnalysisBridge::reportAnalysisCompleted(int64_t totalMs)
{
	each([&](Listener& l) {
		if (l.analysisCompleted) l.analysisCompleted(totalMs);
	});
}

void AnalysisBridge::reportAnalysisCancelled()
{
	each([&](Listener& l) {
		if (l.analysisCancelled) l.analysisCancelled();
	});
}

} // namespace retdec::gui::model
