/**
 * @file src/gui/model/dlg_ai_assistant.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/dlg_ai_assistant.h"

#ifdef RETDEC_GUI_HAS_NEURAL
#include "retdec/neural/inference.h"
#endif

#include <forge/os/os.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <system_error>
#include <thread>

namespace retdec::gui::model {

bool assistantHasNeuralBackend()
{
#ifdef RETDEC_GUI_HAS_NEURAL
	return true;
#else
	return false;
#endif
}

std::string buildAssistantPrompt(
	std::string_view query, std::string_view functionName, std::string_view decompiled, bool contextPending)
{
	if (!contextPending || decompiled.empty()) return std::string(query);
	return std::format("Context: function {}\n{}\n\nQuery: {}", functionName, decompiled, query);
}

std::string assistantErrorText(std::string_view error)
{
	if (error.find("Error:") != std::string_view::npos) return std::string(error);
	return "Error: " + std::string(error);
}

uint32_t chatRoleColor(ChatRole role)
{
	switch (role)
	{
		case ChatRole::User: return 0x44aaff;
		case ChatRole::Assistant: return 0xaaffaa;
		default: return 0x888888;
	}
}

std::map<std::string, std::string> assistantNeuralEnvironment(const MLSettings& ml, bool thinking)
{
	std::map<std::string, std::string> env;
	env["RETDEC_NEURAL_N_GPU_LAYERS"] = ml.inferenceDevice == MLSettings::InferenceDevice::CPU ? "0" : "-1";
	if (ml.contextLength >= 512) env["RETDEC_NEURAL_CTX"] = std::to_string(ml.contextLength);
	if (ml.maxNewTokens > 0) env["RETDEC_NEURAL_MAX_TOKENS"] = std::to_string(ml.maxNewTokens);
	env["RETDEC_NEURAL_TEMPERATURE"] = std::format("{:.4f}", ml.temperature);
	env["RETDEC_NEURAL_TOP_P"] = std::format("{:.4f}", ml.topP);
	env["RETDEC_NEURAL_TOP_K"] = std::to_string(ml.topK);
	env["RETDEC_NEURAL_THINKING"] = thinking ? "1" : "0";
	return env;
}

void applyProcessEnvironmentVariables(const std::map<std::string, std::string>& env)
{
	for (const auto& [k, v]: env)
		forge::os::setenv(k, v);
}

// ─── neural adapter ──────────────────────────────────────────────────────────

#ifdef RETDEC_GUI_HAS_NEURAL
namespace {

class NeuralBackend final : public AssistantBackend {
public:
	explicit NeuralBackend(std::unique_ptr<retdec::neural::Inference> inf): inf_(std::move(inf)) {}
	bool loadModel(const std::string& path, int ctx) override { return inf_ && inf_->loadModel(path, ctx); }
	void unloadModel() override
	{
		if (inf_) inf_->unloadModel();
	}
	bool isLoaded() const override { return inf_ && inf_->isLoaded(); }
	AssistantGenerationResult generate(
		const std::string& prompt,
		const AssistantGenerationParams& p,
		const std::function<bool(std::string_view)>& onPiece) override
	{
		retdec::neural::GenerationConfig cfg;
		cfg.temperature = p.temperature;
		cfg.topP = p.topP;
		cfg.topK = p.topK;
		cfg.maxTokens = p.maxTokens;
		cfg.thinkingMode = p.thinking;
		cfg.reuseKvPrefix = p.reuseKvPrefix;
		const retdec::neural::GenerationResult r = inf_->generate(prompt, cfg);
		AssistantGenerationResult out;
		out.ok = r.ok;
		out.text = r.text;
		out.tokensGenerated = r.tokensGenerated;
		out.error = r.error;
		// retdec::neural returns the whole answer at once: deliver it as one piece.
		if (r.ok && onPiece) onPiece(r.text);
		return out;
	}

private:
	std::unique_ptr<retdec::neural::Inference> inf_;
};

} // namespace
#endif

AssistantBackendFactory defaultAssistantBackendFactory()
{
#ifdef RETDEC_GUI_HAS_NEURAL
	return []() -> std::unique_ptr<AssistantBackend> {
		auto inf = retdec::neural::createForgeInference();
		if (!inf) inf = retdec::neural::createMockInference();
		if (!inf) return nullptr;
		return std::make_unique<NeuralBackend>(std::move(inf));
	};
#else
	return {};
#endif
}

// ─── InferenceWorker ─────────────────────────────────────────────────────────

struct InferenceWorker::State
{
	std::mutex m;
	std::mutex cbM; ///< guards `ready` (cleared by the destructor while a detached thread may still push)
	std::condition_variable cv;      ///< jobs / stop
	std::condition_variable eventCv; ///< events / idle / exited
	std::deque<std::function<void()>> jobs;
	std::vector<InferenceEvent> events;
	std::function<void()> ready;
	AssistantBackendFactory factory;
	std::unique_ptr<AssistantBackend> backend; ///< worker thread only
	AssistantGenerationParams params;
	bool reuseKv = false;
	bool stop = false;
	bool busy = false;
	bool exited = false;
	std::atomic_bool abort{false};

	void push(InferenceEvent e)
	{
		{
			std::lock_guard lk(m);
			events.push_back(std::move(e));
		}
		eventCv.notify_all();
		std::lock_guard lk(cbM);
		if (ready) ready();
	}

	void run()
	{
		for (;;)
		{
			std::function<void()> job;
			{
				std::unique_lock lk(m);
				cv.wait(lk, [&] { return stop || !jobs.empty(); });
				if (stop && jobs.empty()) break;
				job = std::move(jobs.front());
				jobs.pop_front();
				busy = true;
			}
			job();
			{
				std::lock_guard lk(m);
				busy = false;
			}
			eventCv.notify_all();
		}
		backend.reset();
		{
			std::lock_guard lk(m);
			exited = true;
		}
		eventCv.notify_all();
	}

	void doInference(const std::string& prompt)
	{
		// A Stop pressed while nothing ran cancels this request: answer with an
		// empty completion so the panel leaves its busy state (Qt behaviour).
		if (abort.exchange(false))
		{
			push({.kind = InferenceEvent::Kind::Complete});
			return;
		}
		if (!factory)
		{
			push({.kind = InferenceEvent::Kind::Error, .text = std::string(kAssistantNoBackend)});
			return;
		}
		if (!backend || !backend->isLoaded())
		{
			push({.kind = InferenceEvent::Kind::Error, .text = "No model loaded"});
			return;
		}
		AssistantGenerationParams p;
		{
			std::lock_guard lk(m);
			p = params;
			p.reuseKvPrefix = reuseKv;
		}
		const auto t0 = std::chrono::steady_clock::now();
		bool stopped = false;
		const AssistantGenerationResult r = backend->generate(prompt, p, [&](std::string_view piece) {
			if (abort.load())
			{
				stopped = true;
				return false;
			}
			if (!piece.empty()) push({.kind = InferenceEvent::Kind::Token, .text = std::string(piece)});
			return true;
		});
		if (stopped) abort.store(false);
		if (!r.ok && !stopped)
		{
			push({.kind = InferenceEvent::Kind::Error, .text = r.error.empty() ? "generation failed" : r.error});
			return;
		}
		{
			std::lock_guard lk(m);
			reuseKv = true;
		}
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		push({.kind = InferenceEvent::Kind::Complete,
			  .tokens = r.tokensGenerated,
			  .tokPerSec = secs > 0.0 && r.tokensGenerated > 0 ? r.tokensGenerated / secs : 0.0});
	}

	void doLoad(const std::string& path, bool useGpu, int ctxLen)
	{
		forge::os::setenv("RETDEC_NEURAL_N_GPU_LAYERS", useGpu ? "-1" : "0");
		if (!factory)
		{
			push({.kind = InferenceEvent::Kind::SystemMessage, .text = std::string(kAssistantNoBackend)});
			push({.kind = InferenceEvent::Kind::LoadFinished, .ok = false, .path = path});
			return;
		}
		std::error_code ec;
		if (!std::filesystem::exists(std::filesystem::path(path), ec))
		{
			push({.kind = InferenceEvent::Kind::SystemMessage, .text = "Model file missing"});
			push({.kind = InferenceEvent::Kind::LoadFinished, .ok = false, .path = path, .failKind = "missing"});
			return;
		}
		auto b = factory();
		const int resolvedCtx = ctxLen >= 512 ? ctxLen : 4096;
		if (!b || !b->loadModel(path, resolvedCtx))
		{
			push({.kind = InferenceEvent::Kind::LoadFinished, .ok = false, .path = path, .failKind = "load"});
			return;
		}
		backend = std::move(b);
		{
			std::lock_guard lk(m);
			reuseKv = false;
		}
		push({.kind = InferenceEvent::Kind::LoadFinished, .ok = true, .path = path});
	}
};

InferenceWorker::InferenceWorker(AssistantBackendFactory factory): st_(std::make_shared<State>())
{
	st_->factory = std::move(factory);
	std::thread([s = st_] { s->run(); }).detach();
}

InferenceWorker::~InferenceWorker()
{
	st_->abort.store(true);
	{
		std::lock_guard lk(st_->m);
		st_->stop = true;
		st_->jobs.clear();
	}
	{
		std::lock_guard lk(st_->cbM);
		st_->ready = nullptr;
	}
	st_->cv.notify_all();
	// The thread owns a reference to the state: if a generation does not
	// return within the grace period it finishes on its own later.
	std::unique_lock lk(st_->m);
	st_->eventCv.wait_for(lk, std::chrono::seconds(10), [&] { return st_->exited; });
}

void InferenceWorker::setTemperature(float t)
{
	std::lock_guard lk(st_->m);
	st_->params.temperature = t;
}
void InferenceWorker::setTopP(float p)
{
	std::lock_guard lk(st_->m);
	st_->params.topP = p;
}
void InferenceWorker::setTopK(int k)
{
	std::lock_guard lk(st_->m);
	st_->params.topK = k;
}
void InferenceWorker::setMaxTokens(int n)
{
	std::lock_guard lk(st_->m);
	st_->params.maxTokens = n;
}
void InferenceWorker::setThinkingMode(bool on)
{
	std::lock_guard lk(st_->m);
	st_->params.thinking = on;
}
AssistantGenerationParams InferenceWorker::params() const
{
	std::lock_guard lk(st_->m);
	return st_->params;
}

static void enqueue(InferenceWorker::State& s, std::function<void()> job)
{
	{
		std::lock_guard lk(s.m);
		s.jobs.push_back(std::move(job));
	}
	s.cv.notify_all();
}

void InferenceWorker::startInference(std::string prompt)
{
	State* s = st_.get();
	enqueue(*st_, [s, p = std::move(prompt)] { s->doInference(p); });
}

void InferenceWorker::abortInference()
{
	st_->abort.store(true);
}

void InferenceWorker::loadModel(std::string path, bool useGpu, int contextLength)
{
	State* s = st_.get();
	enqueue(*st_, [s, p = std::move(path), useGpu, contextLength] { s->doLoad(p, useGpu, contextLength); });
}

void InferenceWorker::resetKvCache()
{
	State* s = st_.get();
	enqueue(*st_, [s] {
		std::lock_guard lk(s->m);
		s->reuseKv = false;
	});
}

void InferenceWorker::unloadModel()
{
	State* s = st_.get();
	enqueue(*st_, [s] {
		if (s->backend) s->backend->unloadModel();
		s->backend.reset();
	});
}

std::vector<InferenceEvent> InferenceWorker::takeEvents()
{
	std::lock_guard lk(st_->m);
	std::vector<InferenceEvent> out;
	out.swap(st_->events);
	return out;
}

bool InferenceWorker::waitForEvents(std::chrono::milliseconds timeout)
{
	std::unique_lock lk(st_->m);
	return st_->eventCv.wait_for(lk, timeout, [&] { return !st_->events.empty(); });
}

bool InferenceWorker::waitIdle(std::chrono::milliseconds timeout)
{
	std::unique_lock lk(st_->m);
	return st_->eventCv.wait_for(lk, timeout, [&] { return st_->jobs.empty() && !st_->busy; });
}

void InferenceWorker::setEventsReadyCallback(std::function<void()> fn)
{
	std::lock_guard lk(st_->cbM);
	st_->ready = std::move(fn);
}

} // namespace retdec::gui::model
