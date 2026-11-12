/**
 * @file tests/gui/dlg_ai_assistant_panel_test.cpp
 * @brief Port of the Qt ai_assistant_panel_test (AIAssistantPanel and
 *        InferenceWorker) to the FuseUI panel, plus streaming / context /
 *        settings tests that use a fake backend instead of retdec::neural.
 *
 * Model inference is never run on a real model: the default factory has no
 * backend in the standalone build (or loads nothing from a missing path), and
 * the streaming tests inject a scripted backend.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "dlg_test_support.h"

#include "retdec/gui/ui/ai_assistant_panel.h"

#include <forge/os/os.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>

using namespace retdec::gui;
using retdec::gui::test::DlgUi;

namespace {

/// Answers "piece1 piece2 …" token by token; records the prompts it saw.
struct FakeBackendState
{
	std::atomic<int> loads{0};
	std::atomic<bool> loaded{false};
	std::vector<std::string> prompts;
	std::vector<model::AssistantGenerationParams> params;
	std::vector<std::string> pieces{"Hello", " from", " the", " fake", " model."};
	std::atomic<bool> failLoad{false};
	std::mutex m;
};

class FakeBackend final : public model::AssistantBackend {
public:
	explicit FakeBackend(std::shared_ptr<FakeBackendState> s): s_(std::move(s)) {}
	bool loadModel(const std::string&, int) override
	{
		++s_->loads;
		if (s_->failLoad) return false;
		s_->loaded = true;
		return true;
	}
	void unloadModel() override
	{
		s_->loaded = false;
	}
	bool isLoaded() const override
	{
		return s_->loaded;
	}
	model::AssistantGenerationResult generate(
		const std::string& prompt,
		const model::AssistantGenerationParams& p,
		const std::function<bool(std::string_view)>& onPiece) override
	{
		{
			std::lock_guard lk(s_->m);
			s_->prompts.push_back(prompt);
			s_->params.push_back(p);
		}
		model::AssistantGenerationResult r;
		r.ok = true;
		for (const std::string& piece: s_->pieces)
		{
			if (!onPiece(piece)) break;
			r.text += piece;
			++r.tokensGenerated;
		}
		return r;
	}

private:
	std::shared_ptr<FakeBackendState> s_;
};

model::AssistantBackendFactory fakeFactory(std::shared_ptr<FakeBackendState> s)
{
	return [s]() -> std::unique_ptr<model::AssistantBackend> { return std::make_unique<FakeBackend>(s); };
}

struct DlgAIAssistantPanel : ::testing::Test
{
	DlgUi t;
	model::AppSettings settings;
	std::filesystem::path dir = test::makeTempDir("ai");
	~DlgAIAssistantPanel() override
	{
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}
	std::filesystem::path fakeModelFile()
	{
		const auto p = dir / "model.gguf";
		std::ofstream(p) << "GGUF";
		return p;
	}
};

const std::string kMissing = "/__retdec_nonexistent__/nope.gguf";

} // namespace

// ─── Construction (Qt tests 1, 2, 18) ────────────────────────────────────────

TEST_F(DlgAIAssistantPanel, DefaultConstruction)
{
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), {}, settings);
	EXPECT_FALSE(panel.isModelLoaded());
	EXPECT_EQ(panel.panelTitle(), "AI Assistant");
	EXPECT_EQ(panel.modelLabel(), "Model: (none)");
}

TEST_F(DlgAIAssistantPanel, NoBackendExplainsHowToEnable)
{
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), {}, settings);
	t.step();
	const std::string chat = panel.chatText();
	if (model::assistantHasNeuralBackend())
		EXPECT_NE(chat.find("Load a Qwen"), std::string::npos);
	else
	{
		EXPECT_NE(chat.find("This build has no inference backend"), std::string::npos);
		EXPECT_NE(chat.find("-DRETDEC_ENABLE_NEURAL=ON"), std::string::npos);
	}
	t.png("dlg_ai_assistant_initial");
}

// ─── setActiveFunction / clear (Qt tests 3, 4, 9, 12) ────────────────────────

TEST_F(DlgAIAssistantPanel, SetActiveFunctionStoresContext)
{
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), {}, settings);
	panel.setActiveFunction("void foo() { return; }", "foo");
	EXPECT_TRUE(panel.contextPending());
	panel.setActiveFunction("", "noFunction");
	EXPECT_FALSE(panel.contextPending());
	panel.setActiveFunction("void foo() {}", "foo");
	panel.setActiveFunction("void bar() {}", "bar");
	EXPECT_TRUE(panel.contextPending());
}

TEST_F(DlgAIAssistantPanel, ClearResetsContextAndChat)
{
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), {}, settings);
	panel.setActiveFunction("void foo() {}", "foo");
	EXPECT_FALSE(panel.history().empty()); // the start-up system message
	panel.clear();
	EXPECT_FALSE(panel.contextPending());
	EXPECT_TRUE(panel.history().empty());
	EXPECT_TRUE(panel.chatText().empty());
	panel.clear();
	panel.clear();
	panel.setActiveFunction("void b() {}", "b");
	EXPECT_TRUE(panel.contextPending());
}

// ─── Input validation and buttons (Qt tests 5, 14–17) ────────────────────────

TEST_F(DlgAIAssistantPanel, SendEmptyQueryDoesNothing)
{
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), {}, settings);
	const std::size_t before = panel.history().size();
	t.type(panel.queryInput(), "   ");
	t.click(panel.sendButton());
	EXPECT_EQ(panel.history().size(), before);
	EXPECT_FALSE(panel.inferenceRunning());
}

TEST_F(DlgAIAssistantPanel, ButtonStates)
{
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), {}, settings);
	t.step();
	EXPECT_TRUE(t.ui.disabled(panel.stopButton()));
	EXPECT_FALSE(t.ui.disabled(panel.sendButton()));
	EXPECT_EQ(std::string(t.ui.text(panel.loadButton())), "Load");
	EXPECT_EQ(std::string(t.ui.text(panel.sendButton())), "Send");
	EXPECT_EQ(std::string(t.ui.text(panel.stopButton())), "Stop");
}

TEST_F(DlgAIAssistantPanel, GpuButtonTogglesLabel)
{
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), {}, settings);
	EXPECT_FALSE(t.ui.checked(panel.gpuButton()));
	EXPECT_EQ(std::string(t.ui.text(panel.gpuButton())), "GPU: OFF");
	t.click(panel.gpuButton());
	EXPECT_TRUE(t.ui.checked(panel.gpuButton()));
	EXPECT_EQ(std::string(t.ui.text(panel.gpuButton())), "GPU: ON");
	t.click(panel.gpuButton());
	EXPECT_EQ(std::string(t.ui.text(panel.gpuButton())), "GPU: OFF");
}

TEST_F(DlgAIAssistantPanel, SettingsBarHiddenUntilToggled)
{
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), {}, settings);
	t.step();
	EXPECT_FALSE(t.ui.checked(panel.settingsButton()));
	EXPECT_FALSE(t.shown(panel.thinkingCheck()));
	t.click(panel.settingsButton());
	EXPECT_TRUE(panel.settingsBarVisible());
	EXPECT_TRUE(t.shown(panel.thinkingCheck()));
	EXPECT_EQ(std::string(t.ui.text(panel.thinkingCheck())), "Thinking mode");
	t.png("dlg_ai_assistant_settings_bar");
	t.click(panel.settingsButton());
	EXPECT_FALSE(t.shown(panel.thinkingCheck()));
}

TEST_F(DlgAIAssistantPanel, ApplyMlSettingsFromApp)
{
	settings.ml.temperature = 1.25;
	settings.ml.topP = 0.5;
	settings.ml.topK = 7;
	settings.ml.maxNewTokens = 300;
	settings.ml.inferenceDevice = model::MLSettings::InferenceDevice::GPU;
	settings.ml.modelPath = fakeModelFile().string();
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), {}, settings);
	EXPECT_NEAR(t.ui.value(panel.temperatureSpin()), 1.25f, 1e-4f);
	EXPECT_NEAR(t.ui.value(panel.topPSpin()), 0.5f, 1e-4f);
	EXPECT_EQ(t.ui.value(panel.topKSpin()), 7.f);
	EXPECT_EQ(t.ui.value(panel.maxTokensSpin()), 300.f);
	EXPECT_TRUE(panel.gpuEnabled());
	EXPECT_EQ(panel.modelLabel(), "Model: " + settings.ml.modelPath + " (not loaded)");
	const auto p = panel.worker().params();
	EXPECT_FLOAT_EQ(p.temperature, 1.25f);
	EXPECT_EQ(p.topK, 7);
	EXPECT_EQ(p.maxTokens, 300);
	EXPECT_EQ(forge::os::getenv("RETDEC_NEURAL_TOP_K", ""), "7");
	EXPECT_EQ(forge::os::getenv("RETDEC_NEURAL_N_GPU_LAYERS", ""), "-1");
	EXPECT_EQ(forge::os::getenv("RETDEC_NEURAL_TEMPERATURE", ""), "1.2500");

	// Settings bar edits reach the worker; thinking mode sets the variable.
	panel.setSettingsBarVisible(true);
	t.ui.setFocus(panel.topKSpin(), fuse::ui::FocusReason::Keyboard);
	t.key(fuse::ui::Key::Up);
	EXPECT_EQ(panel.worker().params().topK, 8);
	t.click(panel.thinkingCheck());
	EXPECT_TRUE(panel.worker().params().thinking);
	EXPECT_EQ(forge::os::getenv("RETDEC_NEURAL_THINKING", ""), "1");
}

// ─── unloadModel / loads (Qt tests 13, 20, 21, 23) ───────────────────────────

TEST_F(DlgAIAssistantPanel, UnloadModelUpdatesState)
{
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), {}, settings);
	panel.unloadModel();
	EXPECT_FALSE(panel.isModelLoaded());
	EXPECT_EQ(panel.modelLabel(), "Model: (none)");
}

TEST_F(DlgAIAssistantPanel, AsyncLoadInvalidUpdatesLabel)
{
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), model::defaultAssistantBackendFactory(), settings);
	ASSERT_TRUE(panel.loadModel(kMissing, false));
	EXPECT_TRUE(t.waitUntil([&] { return panel.modelLabel().find("Load failed") != std::string::npos; }, 120000))
		<< panel.modelLabel();
	EXPECT_FALSE(panel.isModelLoaded());
}

TEST_F(DlgAIAssistantPanel, DoubleInvalidLoadStillConsistent)
{
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), model::defaultAssistantBackendFactory(), settings);
	panel.loadModel("/__retdec_a__/1.gguf", false);
	EXPECT_TRUE(t.waitUntil([&] { return panel.modelLabel() == "Load failed"; }, 120000));
	panel.loadModel("/__retdec_b__/2.gguf", false);
	EXPECT_TRUE(t.waitUntil([&] { return panel.modelLabel() == "Load failed"; }, 120000));
	EXPECT_FALSE(panel.isModelLoaded());
}

TEST_F(DlgAIAssistantPanel, WorkerLoadInvalidReportsFailure)
{
	model::InferenceWorker worker(model::defaultAssistantBackendFactory());
	worker.loadModel(kMissing, false, 512);
	ASSERT_TRUE(worker.waitIdle(std::chrono::seconds(120)));
	bool sawLoad = false;
	for (const auto& e: worker.takeEvents())
		if (e.kind == model::InferenceEvent::Kind::LoadFinished)
		{
			sawLoad = true;
			EXPECT_FALSE(e.ok);
			EXPECT_EQ(e.path, kMissing);
		}
	EXPECT_TRUE(sawLoad);
	EXPECT_EQ(forge::os::getenv("RETDEC_NEURAL_N_GPU_LAYERS", ""), "0");
}

// ─── Inference without a model (Qt tests 10, 19, 22) ─────────────────────────

TEST_F(DlgAIAssistantPanel, WorkerEmitsErrorWhenNoPipeline)
{
	model::InferenceWorker worker(model::defaultAssistantBackendFactory());
	worker.startInference("Hello");
	ASSERT_TRUE(worker.waitIdle(std::chrono::seconds(10)));
	const auto events = worker.takeEvents();
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0].kind, model::InferenceEvent::Kind::Error);
	EXPECT_FALSE(events[0].text.empty());
}

TEST_F(DlgAIAssistantPanel, WorkerAbortAndSettersDoNotCrash)
{
	model::InferenceWorker worker;
	worker.abortInference();
	worker.setTemperature(0.5f);
	worker.setTopP(0.8f);
	worker.setMaxTokens(128);
	worker.setThinkingMode(true);
	EXPECT_EQ(worker.params().maxTokens, 128);
	EXPECT_TRUE(worker.params().thinking);
	// The pending abort cancels the next request with an empty completion.
	worker.startInference("x");
	ASSERT_TRUE(worker.waitIdle(std::chrono::seconds(10)));
	const auto events = worker.takeEvents();
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0].kind, model::InferenceEvent::Kind::Complete);
}

TEST_F(DlgAIAssistantPanel, SendWithoutModelSurfacesError)
{
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), model::defaultAssistantBackendFactory(), settings);
	t.type(panel.queryInput(), "hello");
	t.click(panel.sendButton());
	EXPECT_EQ(panel.lastPrompt(), "hello"); // sent (the worker may already have answered)
	EXPECT_TRUE(t.waitUntil(
		[&] {
			const std::string c = panel.chatText();
			return c.find("No model loaded") != std::string::npos || c.find("Error:") != std::string::npos;
		},
		10000))
		<< panel.chatText();
	EXPECT_FALSE(panel.inferenceRunning());
	EXPECT_FALSE(t.ui.disabled(panel.sendButton()));
	EXPECT_TRUE(t.ui.disabled(panel.stopButton()));
	EXPECT_TRUE(ui::dlg::editText(t.ui, panel.queryInput()).empty());
}

// ─── ChatMessage (Qt tests 11) ───────────────────────────────────────────────

TEST(DlgAIAssistantModel, ChatMessageAndRoles)
{
	model::ChatMessage msg;
	msg.role = model::ChatRole::User;
	msg.text = "Hello";
	msg.isComplete = true;
	EXPECT_EQ(msg.role, model::ChatRole::User);
	EXPECT_EQ(msg.text, "Hello");
	EXPECT_TRUE(msg.isComplete);
	EXPECT_NE(model::ChatRole::User, model::ChatRole::Assistant);
	EXPECT_NE(model::ChatRole::Assistant, model::ChatRole::System);
	EXPECT_NE(model::ChatRole::System, model::ChatRole::Thinking);
	EXPECT_EQ(model::chatRoleColor(model::ChatRole::User), 0x44aaffu);
	EXPECT_EQ(model::chatRoleColor(model::ChatRole::Assistant), 0xaaffaau);
}

TEST(DlgAIAssistantModel, PromptAndErrorText)
{
	EXPECT_EQ(
		model::buildAssistantPrompt("why?", "foo", "int foo(){}", true),
		"Context: function foo\nint foo(){}\n\nQuery: why?");
	EXPECT_EQ(model::buildAssistantPrompt("why?", "foo", "int foo(){}", false), "why?");
	EXPECT_EQ(model::buildAssistantPrompt("why?", "foo", "", true), "why?");
	EXPECT_EQ(model::assistantErrorText("No model loaded"), "Error: No model loaded");
	EXPECT_EQ(model::assistantErrorText("Error: x"), "Error: x");
}

TEST(DlgAIAssistantModel, NeuralEnvironment)
{
	model::MLSettings ml;
	ml.inferenceDevice = model::MLSettings::InferenceDevice::CPU;
	ml.contextLength = 256; // < 512: not published
	ml.maxNewTokens = 0;
	auto env = model::assistantNeuralEnvironment(ml, true);
	EXPECT_EQ(env["RETDEC_NEURAL_N_GPU_LAYERS"], "0");
	EXPECT_FALSE(env.contains("RETDEC_NEURAL_CTX"));
	EXPECT_FALSE(env.contains("RETDEC_NEURAL_MAX_TOKENS"));
	EXPECT_EQ(env["RETDEC_NEURAL_TOP_P"], "0.9000");
	EXPECT_EQ(env["RETDEC_NEURAL_THINKING"], "1");
	ml.inferenceDevice = model::MLSettings::InferenceDevice::Auto;
	ml.contextLength = 8192;
	env = model::assistantNeuralEnvironment(ml, false);
	EXPECT_EQ(env["RETDEC_NEURAL_N_GPU_LAYERS"], "-1");
	EXPECT_EQ(env["RETDEC_NEURAL_CTX"], "8192");
}

// ─── With a (fake) model: loading, context prompt, streaming ─────────────────

TEST_F(DlgAIAssistantPanel, StreamsAnswerWithFunctionContext)
{
	auto state = std::make_shared<FakeBackendState>();
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), fakeFactory(state), settings);
	const auto file = fakeModelFile();
	panel.loadModel(file.string(), true);
	ASSERT_TRUE(t.waitUntil([&] { return panel.isModelLoaded(); }));
	EXPECT_EQ(panel.modelLabel(), "Model: " + file.string());
	EXPECT_TRUE(panel.gpuEnabled());

	panel.setActiveFunction("int add(int a, int b) { return a + b; }", "add");
	t.type(panel.queryInput(), "What does it do?");
	t.key(fuse::ui::Key::Enter);
	EXPECT_EQ(
		panel.lastPrompt(),
		"Context: function add\nint add(int a, int b) { return a + b; }\n\nQuery: What does it do?");
	ASSERT_TRUE(t.waitUntil([&] { return !panel.inferenceRunning(); }));
	ASSERT_FALSE(panel.history().empty());
	const model::ChatMessage& last = panel.history().back();
	EXPECT_EQ(last.role, model::ChatRole::Assistant);
	EXPECT_EQ(last.text, "Hello from the fake model.");
	EXPECT_TRUE(last.isComplete);
	EXPECT_NE(panel.chatText().find("Hello from the fake model."), std::string::npos);
	EXPECT_EQ(panel.statusText().rfind("5 tokens", 0), 0u);
	t.png("dlg_ai_assistant_chat");

	// Second query: no context any more, KV prefix reuse on.
	t.type(panel.queryInput(), "And again?");
	t.click(panel.sendButton());
	EXPECT_EQ(panel.lastPrompt(), "And again?");
	ASSERT_TRUE(t.waitUntil([&] { return !panel.inferenceRunning(); }));
	std::lock_guard lk(state->m);
	ASSERT_EQ(state->params.size(), 2u);
	EXPECT_FALSE(state->params[0].reuseKvPrefix);
	EXPECT_TRUE(state->params[1].reuseKvPrefix);
}

TEST_F(DlgAIAssistantPanel, StreamingOffShowsAnswerOnlyWhenComplete)
{
	settings.ml.streamOutput = false;
	auto state = std::make_shared<FakeBackendState>();
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), fakeFactory(state), settings);
	panel.loadModel(fakeModelFile().string());
	ASSERT_TRUE(t.waitUntil([&] { return panel.isModelLoaded(); }));
	// Run the worker without pumping, then deliver only the token events.
	panel.worker().startInference("q");
	ASSERT_TRUE(panel.worker().waitIdle(std::chrono::seconds(10)));
	auto events = panel.worker().takeEvents();
	ASSERT_GE(events.size(), 2u);
	EXPECT_EQ(events.back().kind, model::InferenceEvent::Kind::Complete);
	EXPECT_EQ(events.front().kind, model::InferenceEvent::Kind::Token);
	EXPECT_EQ(events.front().text, "Hello");
}

TEST_F(DlgAIAssistantPanel, LoadFailureFromBackend)
{
	auto state = std::make_shared<FakeBackendState>();
	state->failLoad = true;
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), fakeFactory(state), settings);
	panel.loadModel(fakeModelFile().string());
	ASSERT_TRUE(t.waitUntil([&] { return panel.modelLabel() == "Load failed"; }));
	EXPECT_FALSE(panel.isModelLoaded());
	EXPECT_EQ(state->loads.load(), 1);
	// Missing file: never reaches the backend.
	panel.loadModel(kMissing);
	ASSERT_TRUE(panel.worker().waitIdle(std::chrono::seconds(10)));
	t.step();
	EXPECT_EQ(state->loads.load(), 1);
	EXPECT_NE(panel.chatText().find("Model file missing"), std::string::npos);
}

TEST_F(DlgAIAssistantPanel, LoadButtonOpensGgufPrompt)
{
	auto state = std::make_shared<FakeBackendState>();
	ui::AIAssistantPanel panel(t.ui, t.ui.root(), fakeFactory(state), settings);
	const auto file = fakeModelFile();
	std::ofstream(dir / "notes.txt") << "x";
	settings.ml.modelPath = file.string();
	t.click(panel.loadButton());
	ASSERT_TRUE(panel.filePrompt().isOpen());
	ASSERT_EQ(fuse::ui::widgets::fileDialogEntryCount(t.ui, panel.filePrompt().dialog()), 1u);
	EXPECT_EQ(fuse::ui::widgets::fileDialogEntry(t.ui, panel.filePrompt().dialog(), 0), "model.gguf");
	fuse::ui::widgets::fileDialogActivate(t.ui, panel.filePrompt().dialog(), 0);
	ASSERT_TRUE(t.waitUntil([&] { return panel.isModelLoaded(); }));
}

TEST_F(DlgAIAssistantPanel, DestroyWhileGeneratingIsSafe)
{
	auto state = std::make_shared<FakeBackendState>();
	state->pieces.assign(2000, "x");
	{
		ui::AIAssistantPanel panel(t.ui, t.ui.root(), fakeFactory(state), settings);
		panel.loadModel(fakeModelFile().string());
		ASSERT_TRUE(t.waitUntil([&] { return panel.isModelLoaded(); }));
		t.type(panel.queryInput(), "long");
		t.click(panel.stopButton()); // disabled: no effect
		t.click(panel.sendButton());
	}
	t.step(3); // posted events for the destroyed panel are dropped
	SUCCEED();
}
