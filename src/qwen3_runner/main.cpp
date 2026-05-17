/**
 * @file src/qwen3_runner/main.cpp
 * @brief retdec-qwen3-runner — download a Qwen3 GGUF model and run it.
 *
 * Usage:
 *   retdec-qwen3-runner <model.gguf> [options]
 *
 * Options:
 *   --prompt/-p <text>      User prompt (default: interactive REPL)
 *   --max-tokens/-n <n>     Max new tokens to generate (default: 512)
 *   --temperature/-t <f>    Sampling temperature (default: 0.6, 0 = greedy)
 *   --top-p <f>             Nucleus sampling (default: 0.9)
 *   --top-k <n>             Top-K sampling (default: 0 = disabled)
 *   --rep-penalty <f>       Repetition penalty (default: 1.1)
 *   --seed <n>              RNG seed (default: 42)
 *   --thinking              Enable Qwen3 chain-of-thought (<think> mode)
 *   --no-chat               Tokenize prompt verbatim, no ChatML template
 *   --tokenizer/-T <path>   Optional separate tokenizer.json file
 *   --system <text>         System prompt for the conversation
 *   --info                  Print model info and exit
 *   --list-tensors          Print all tensor names/dtypes and exit
 *   --version               Print version and exit
 *
 * Examples:
 *   # One-shot generation
 *   retdec-qwen3-runner qwen3-1.7b-q4_k_m.gguf -p "What is main()?" -n 200
 *
 *   # Interactive REPL
 *   retdec-qwen3-runner qwen3-1.7b-q4_k_m.gguf
 *
 *   # Greedy decode (no randomness)
 *   retdec-qwen3-runner qwen3-1.7b-q4_k_m.gguf -p "Count to 5" -t 0
 */

#include "retdec/qwen3/qwen3_model.h"
#include "retdec/qwen3/qwen3_cuda.h"
#include "retdec/qwen3/qwen3_weights.h"
#include "retdec/cuda_accel/cuda_context.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace retdec::qwen3;

// ─── Argument parsing ─────────────────────────────────────────────────────────

struct CliArgs {
    std::string modelPath;
    std::string prompt;
    std::string system    = "You are a helpful assistant.";
    std::string tokenizerPath;
    int         maxTokens = 512;
    float       temperature = 0.6f;
    float       topP      = 0.9f;
    int         topK      = 0;
    float       repPenalty = 1.1f;
    int         seed      = 42;
    bool        thinking  = false;
    bool        noChat    = false;
    bool        infoOnly  = false;
    bool        listTensors = false;
    bool        listDevices = false;
    bool        printVersion = false;
    bool        interactive = true;
    // CUDA options
    bool        useCUDA     = false;
    float       gpuFraction = 0.80f;
};

static void printUsage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s <model.gguf> [options]\n\n"
        "Options:\n"
        "  --prompt/-p <text>     User prompt (if omitted: interactive REPL)\n"
        "  --max-tokens/-n <n>    Max new tokens (default: 512)\n"
        "  --temperature/-t <f>   Temperature (default: 0.6, 0=greedy)\n"
        "  --top-p <f>            Nucleus sampling (default: 0.9)\n"
        "  --top-k <n>            Top-K (default: 0 = off)\n"
        "  --rep-penalty <f>      Repetition penalty (default: 1.1)\n"
        "  --seed <n>             RNG seed (default: 42)\n"
        "  --thinking             Enable chain-of-thought\n"
        "  --no-chat              Tokenize prompt verbatim (no ChatML)\n"
        "  --tokenizer/-T <path>  Load tokenizer from file\n"
        "  --system <text>        System prompt\n"
        "  --info                 Print model info and exit\n"
        "  --list-tensors         Print tensor list and exit\n"
        "  --list-devices         Print CUDA devices and exit\n"
        "  --cuda                 Enable GPU inference via CUDA\n"
        "  --gpu-fraction <f>     Fraction of each GEMV on GPU (default: 0.8)\n"
        "  --version              Print version and exit\n",
        argv0);
}

static CliArgs parseArgs(int argc, char** argv) {
    CliArgs a;
    if (argc < 2) { printUsage(argv[0]); std::exit(1); }

    a.modelPath = argv[1];
    if (a.modelPath == "--version" || a.modelPath == "-v") {
        a.printVersion = true;
        return a;
    }

    for (int i = 2; i < argc; ++i) {
        auto arg = std::string(argv[i]);
        auto next = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            std::fprintf(stderr, "Missing argument for %s\n", argv[i]);
            std::exit(1);
        };
        if (arg == "--prompt" || arg == "-p") {
            a.prompt = next();
            a.interactive = false;
        } else if (arg == "--max-tokens" || arg == "-n") {
            a.maxTokens = std::stoi(next());
        } else if (arg == "--temperature" || arg == "-t") {
            a.temperature = std::stof(next());
        } else if (arg == "--top-p") {
            a.topP = std::stof(next());
        } else if (arg == "--top-k") {
            a.topK = std::stoi(next());
        } else if (arg == "--rep-penalty") {
            a.repPenalty = std::stof(next());
        } else if (arg == "--seed") {
            a.seed = std::stoi(next());
        } else if (arg == "--thinking") {
            a.thinking = true;
        } else if (arg == "--no-chat") {
            a.noChat = true;
        } else if (arg == "--tokenizer" || arg == "-T") {
            a.tokenizerPath = next();
        } else if (arg == "--system") {
            a.system = next();
        } else if (arg == "--info") {
            a.infoOnly = true;
        } else if (arg == "--list-tensors") {
            a.listTensors = true;
        } else if (arg == "--list-devices") {
            a.listDevices = true;
        } else if (arg == "--cuda") {
            a.useCUDA = true;
        } else if (arg == "--gpu-fraction") {
            a.gpuFraction = std::stof(next());
        } else if (arg == "--version") {
            a.printVersion = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            printUsage(argv[0]);
            std::exit(1);
        }
    }
    return a;
}

// ─── Info / tensor listing ────────────────────────────────────────────────────

static void printInfo(const Qwen3Model& model) {
    const auto& c = model.config();
    std::printf("Model info:\n");
    std::printf("  Hidden size:        %d\n",  c.hiddenSize);
    std::printf("  Num layers:         %d\n",  c.numLayers);
    std::printf("  Num heads (Q):      %d\n",  c.numHeads);
    std::printf("  Num heads (KV):     %d\n",  c.numKvHeads);
    std::printf("  Head dim:           %d\n",  c.headDim);
    std::printf("  Intermediate:       %d\n",  c.intermediateSize);
    std::printf("  Vocab size:         %d\n",  c.vocabSize);
    std::printf("  Max context:        %d\n",  c.maxPositionEmbeddings);
    std::printf("  RoPE theta:         %.0f\n",c.ropeTheta);
    std::printf("  MoE:                %s\n",  c.isMoE() ? "yes" : "no");
    std::printf("  Est. params (M):    %.0f\n",c.estimatedParamsM());
    std::printf("  Tokenizer loaded:   %s\n",
                model.tokenizer().isLoaded() ? "yes" : "no");
    std::printf("  Vocab entries:      %zu\n", model.tokenizer().vocabSize());
}

static void printTensors(const std::string& path) {
    Qwen3Weights w;
    if (!w.openGguf(path)) {
        std::fprintf(stderr, "Cannot open: %s\n", path.c_str());
        return;
    }
    std::printf("%-60s  %-10s  %s\n", "Name", "DType", "Shape");
    std::printf("%-60s  %-10s  %s\n",
                std::string(60, '-').c_str(),
                std::string(10, '-').c_str(),
                "-----");
    for (auto& ti : w.tensors()) {
        std::printf("%-60s  %-10s  [", ti.name.c_str(),
                    ggufDtypeName(ti.dtype));
        for (uint32_t d = 0; d < ti.nDims; ++d) {
            if (d > 0) std::printf(", ");
            std::printf("%llu", (unsigned long long)ti.dims[d]);
        }
        std::printf("]\n");
    }
}

// ─── Generation ───────────────────────────────────────────────────────────────

static GenerateOptions makeOpts(const CliArgs& a) {
    GenerateOptions o;
    o.maxNewTokens      = a.maxTokens;
    o.temperature       = a.temperature;
    o.topP              = a.topP;
    o.topK              = a.topK;
    o.repetitionPenalty = a.repPenalty;
    o.seed              = a.seed;
    o.enableThinking    = a.thinking;
    // Stream output token-by-token
    o.textCallback = [](const std::string& piece) {
        std::cout << piece << std::flush;
    };
    return o;
}

static void runOneShot(Qwen3Model& model, const CliArgs& a) {
    auto opts = makeOpts(a);
    GenerateResult res;

    if (a.noChat) {
        // Tokenize verbatim
        auto ids = model.tokenizer().encode(a.prompt);
        res = model.generateFromIds(ids, opts);
    } else {
        // Apply ChatML template
        std::string fullPrompt =
            "<|im_start|>system\n" + a.system + "<|im_end|>\n"
            "<|im_start|>user\n"   + a.prompt + "<|im_end|>\n"
            "<|im_start|>assistant\n";
        if (a.thinking) fullPrompt += "<think>\n";
        auto ids = model.tokenizer().encode(fullPrompt);
        res = model.generateFromIds(ids, opts);
    }

    std::cout << "\n";
    std::fprintf(stderr,
                 "\n[%d prompt + %d new tokens, %.1f tok/s%s]\n",
                 res.promptTokens, res.newTokens, res.tokensPerSec,
                 res.hitEos ? ", EOS" : "");
}

static void runRepl(Qwen3Model& model, const CliArgs& a) {
    std::printf("Qwen3 interactive session (type 'quit' or Ctrl-C to exit)\n");
    std::printf("System: %s\n\n", a.system.c_str());

    // Build initial system part of the context
    std::string systemTurn =
        "<|im_start|>system\n" + a.system + "<|im_end|>\n";
    TokenIds context = model.tokenizer().encode(systemTurn);

    while (true) {
        std::printf("You> ");
        std::fflush(stdout);
        std::string line;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line == "quit" || line == "exit") break;
        if (line == "/reset") {
            context = model.tokenizer().encode(systemTurn);
            std::printf("[Context reset]\n\n");
            continue;
        }

        // Append user turn
        std::string userTurn =
            "<|im_start|>user\n" + line + "<|im_end|>\n"
            "<|im_start|>assistant\n";
        if (a.thinking) userTurn += "<think>\n";
        auto userIds = model.tokenizer().encode(userTurn);
        context.insert(context.end(), userIds.begin(), userIds.end());

        // Generate from the full context
        std::printf("Assistant> ");
        std::fflush(stdout);

        auto opts = makeOpts(a);
        auto res  = model.generateFromIds(context, opts);
        std::cout << "\n";
        std::fprintf(stderr, "[%d tok, %.1f tok/s]\n\n",
                     res.newTokens, res.tokensPerSec);

        // Append assistant response to context for multi-turn
        auto assistIds = model.tokenizer().encode(
            res.text + "<|im_end|>\n");
        context.insert(context.end(), assistIds.begin(), assistIds.end());

        // Trim context if it's getting too long
        int maxCtx = model.config().maxPositionEmbeddings;
        if (static_cast<int>(context.size()) > maxCtx - 256) {
            // Keep system + last ~half the context
            auto sysLen = model.tokenizer().encode(systemTurn).size();
            int  keep   = maxCtx / 2;
            if (static_cast<int>(context.size()) - keep >
                    static_cast<int>(sysLen)) {
                auto newCtx = TokenIds(context.begin(),
                                       context.begin() + sysLen);
                auto tail   = TokenIds(context.end() - keep, context.end());
                newCtx.insert(newCtx.end(), tail.begin(), tail.end());
                context = std::move(newCtx);
                std::fprintf(stderr, "[Context trimmed to %zu tokens]\n",
                             context.size());
            }
        }
    }
    std::printf("\nBye!\n");
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    CliArgs a = parseArgs(argc, argv);

    if (a.printVersion) {
        std::printf("retdec-qwen3-runner 1.0.0\n");
        return 0;
    }

    if (a.listTensors) {
        printTensors(a.modelPath);
        return 0;
    }

    if (a.listDevices) {
#ifdef RETDEC_HAS_CUDA
        retdec::cuda_accel::CUDAContext ctx;
        ctx.initialize();
        auto& devs = ctx.allDevices();
        if (devs.empty()) {
            std::printf("No CUDA devices found.\n");
        } else {
            std::printf("%-4s  %-40s  %-10s  %s\n", "#", "Name", "GlobalMem", "SMs");
            for (int i = 0; i < static_cast<int>(devs.size()); i++) {
                auto& d = devs[i];
                std::printf("%-4d  %-40s  %6.0f MB  %d\n",
                    i, d.name.c_str(),
                    d.globalMemBytes / 1e6,
                    d.multiProcessorCount);
            }
        }
#else
        std::printf("CUDA support not compiled in.\n");
#endif
        return 0;
    }

    // Load model
    std::fprintf(stderr, "Loading %s ...\n", a.modelPath.c_str());
    Qwen3Model model;
    if (!model.loadGguf(a.modelPath)) {
        std::fprintf(stderr, "Error: %s\n", model.lastError().c_str());
        return 1;
    }
    std::fprintf(stderr, "Loaded  (%.0fM params, vocab=%d, ctx=%d)\n",
                 model.config().estimatedParamsM(),
                 model.config().vocabSize,
                 model.config().maxPositionEmbeddings);

    // Optional external tokenizer
    if (!a.tokenizerPath.empty()) {
        if (!model.loadTokenizer(a.tokenizerPath)) {
            std::fprintf(stderr, "Warning: %s\n", model.lastError().c_str());
        }
    }

    if (!model.tokenizer().isLoaded()) {
        std::fprintf(stderr,
            "Error: No tokenizer available.\n"
            "       The GGUF file does not contain an embedded vocabulary.\n"
            "       Provide one with --tokenizer <tokenizer.json>\n");
        return 1;
    }

    // Enable CUDA if requested
    if (a.useCUDA) {
        std::fprintf(stderr, "Initialising CUDA (GPU fraction=%.0f%%)...\n",
                     a.gpuFraction * 100.f);
        if (!model.enableCUDA(a.gpuFraction)) {
            std::fprintf(stderr, "Warning: CUDA unavailable — %s\n"
                                 "         Falling back to CPU-only.\n",
                         model.lastError().c_str());
        }
    }

    if (a.infoOnly) {
        printInfo(model);
        return 0;
    }

    // Run
    if (a.interactive) {
        runRepl(model, a);
    } else {
        runOneShot(model, a);
    }

    return 0;
}
