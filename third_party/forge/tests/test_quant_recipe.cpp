// Mixed-precision recipe: llama.cpp llama_tensor_get_type() port (Q4_K_M / Q5_K_M) and FORGE's extensions.
#include "check.hpp"
#include <forge/import/quant_recipe.hpp>

using namespace forge;
using forge::import::quant_recipe;
using forge::import::recipe_type;

TEST(use_more_bits_matches_llama) {
    // n = 32: layers 0-3 and 28-31, plus (i-4)%3==2 -> 6, 9, ..., 27
    std::vector<int> more;
    for (int i = 0; i < 32; ++i) if (import::use_more_bits(i, 32)) more.push_back(i);
    std::vector<int> want{0, 1, 2, 3, 6, 9, 12, 15, 18, 21, 24, 27, 28, 29, 30, 31};
    CHECK(more == want);
}

TEST(llama_q4_k_m) {
    quant_recipe r; r.ftype = "q4_k_m"; r.recipe = "llama"; r.n_layer = 32; r.n_attn_v = 32;
    CHECK(recipe_type(r, "lm_head", 4096) == "q6_k");
    CHECK(recipe_type(r, "tok_emb", 4096) == "q4_k");
    CHECK(recipe_type(r, "layer.0.wv", 4096, false, 0) == "q6_k");
    CHECK(recipe_type(r, "layer.4.wv", 4096, false, 4) == "q4_k");
    CHECK(recipe_type(r, "layer.6.wv", 4096, false, 6) == "q6_k");
    CHECK(recipe_type(r, "layer.31.w_down", 14336) == "q6_k");
    CHECK(recipe_type(r, "layer.5.w_down", 14336) == "q4_k");
    CHECK(recipe_type(r, "layer.5.wq", 4096) == "q4_k");
    CHECK(recipe_type(r, "layer.5.wo", 4096) == "q4_k");
    CHECK(recipe_type(r, "layer.5.w_gate", 4096) == "q4_k");
    CHECK(recipe_type(r, "layer.5.w_qkv", 4096) == "q5_k");
    CHECK(recipe_type(r, "lm_head", 4000) == "q8_0");          // rows not whole k-quant blocks
    CHECK(recipe_type(r, "layer.5.wq", 4000) == "q8_0");       // shape fallback
    r.ftype = "q5_k_m";
    CHECK(recipe_type(r, "layer.5.wq", 4096) == "q5_k");
    CHECK(recipe_type(r, "layer.5.w_qkv", 4096) == "q6_k");
    CHECK(recipe_type(r, "layer.0.w_down", 4096) == "q6_k");
    r.n_expert = 8;
    CHECK(recipe_type(r, "layer.5.wv", 4096, false, 5) == "q8_0");
    CHECK(recipe_type(r, "layer.5.wo", 4096) == "q5_k");
}

TEST(overrides_and_pure) {
    quant_recipe r; r.ftype = "q4_k_m"; r.recipe = "llama"; r.n_layer = 32;
    r.output_type = "q8_0"; r.token_embedding_type = "f16";
    CHECK(recipe_type(r, "lm_head", 4096) == "q8_0");
    CHECK(recipe_type(r, "tok_emb", 4096) == "f16");
    r.tensor_types = {{"layer\\.[0-9]+\\.wv$", "q8_0"}, {"w_down", "q5_k"}};
    CHECK(recipe_type(r, "layer.4.wv", 4096, false, 4) == "q8_0");
    CHECK(recipe_type(r, "layer.0.w_down", 4096) == "q5_k");
    quant_recipe p; p.ftype = "q4_k_m"; p.recipe = "llama"; p.n_layer = 32; p.pure = true;
    CHECK(recipe_type(p, "lm_head", 4096) == "q4_k");
    CHECK(recipe_type(p, "layer.0.wv", 4096, false, 0) == "q4_k");
    CHECK_THROWS(import::parse_tensor_type("novalue"));
}

TEST(forge_extension) {
    quant_recipe r; r.ftype = "q4_k_m"; r.n_layer = 32; r.n_attn_v = 8;   // Qwen3.5-style: 8 full-attention layers
    CHECK(recipe_type(r, "lm_head", 4096) == "q8_0");
    CHECK(recipe_type(r, "tok_emb", 4096) == "q8_0");
    CHECK(recipe_type(r, "layer.0.w_qkv", 4096, true) == "q6_k");
    CHECK(recipe_type(r, "layer.0.w_z", 4096, true) == "q6_k");
    CHECK(recipe_type(r, "layer.0.wo", 4096, true) == "q6_k");
    CHECK(recipe_type(r, "layer.3.wo", 4096, false) == "q6_k");
    CHECK(recipe_type(r, "layer.10.w_gate", 4096) == "q4_k");
    CHECK(recipe_type(r, "layer.3.wv", 4096, false, 0) == "q6_k");   // index among the 8 V projections
    CHECK(recipe_type(r, "layer.11.wv", 4096, false, 2) == "q4_k");
    r.head_bf16 = true;
    CHECK(recipe_type(r, "lm_head", 4096) == "bf16");
    CHECK(recipe_type(r, "tok_emb", 4096) == "bf16");
}
