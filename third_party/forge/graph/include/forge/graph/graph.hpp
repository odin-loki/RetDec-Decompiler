#pragma once
// Umbrella header for graph: IR, passes, codecs, memory planning, CPU and Vulkan lowerings, and plans/executor.
// autodiff.hpp, kv_codec.hpp and rpc.hpp are included separately. Guide: docs/graph.md.
#include <forge/graph/codecs.hpp>
#include <forge/graph/ir.hpp>
#include <forge/graph/passes.hpp>
#include <forge/graph/memplan.hpp>
#include <forge/graph/kernels.hpp>
#include <forge/graph/plan.hpp>
#include <forge/graph/vulkan.hpp>
