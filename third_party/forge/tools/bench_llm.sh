#!/usr/bin/env bash
# LLM inference benchmark: FORGE vs llama.cpp on the same GGUF.
#
#   tools/bench_llm.sh --gguf model.gguf [--fpk model.fpk] [--forge build/release/forge]
#                      [--llama-bench ~/work/llama.cpp/build/bin/llama-bench] [-p 512] [-n 128] [-r 3]
#                      [--device cuda] [--codec q4_k] [--nsys out_prefix] [--no-llama]
#
# Reports prefill (pp) and decode (tg) tok/s for both engines, plus FORGE's GPU driver call counts per decode
# token (kernel launches, kernels captured into CUDA graphs, graph launches, DtoH/HtoD/DtoD copies, syncs) from
# `forge bench`. FORGE runs with and without FORGE_GRAPHS=1. With --nsys, each FORGE run is also captured with
# Nsight Systems (cuda,nvtx traces) and a kernel summary is printed. Not part of ctest: it needs a GPU and a model.
set -u
gguf= fpk= forge=build/release/forge llama=${LLAMA_BENCH:-$HOME/work/llama.cpp/build/bin/llama-bench}
p=512 n=128 r=3 dev=cuda codec=q4_k nsys= nollama=0
while [[ $# -gt 0 ]]; do
  case $1 in
    --gguf) gguf=$2; shift ;; --fpk) fpk=$2; shift ;; --forge) forge=$2; shift ;; --llama-bench) llama=$2; shift ;;
    -p) p=$2; shift ;; -n) n=$2; shift ;; -r) r=$2; shift ;; --device) dev=$2; shift ;; --codec) codec=$2; shift ;;
    --nsys) nsys=$2; shift ;; --no-llama) nollama=1 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "bench_llm: unknown argument $1" >&2; exit 2 ;;
  esac; shift
done
[[ -n $gguf || -n $fpk ]] || { echo "bench_llm: need --gguf and/or --fpk" >&2; exit 2; }
model=${fpk:-$gguf}
echo "== FORGE ($(git -C "$(dirname "$0")" rev-parse --short HEAD 2>/dev/null || echo ?)) model=$model device=$dev p=$p n=$n"
for graphs in 0 1; do
  for ((i = 1; i <= r; ++i)); do
    FORGE_DEVICE=$dev FORGE_GRAPHS=$graphs "$forge" bench "$model" --codec "$codec" -p "$p" -n "$n" 2>/dev/null |
      sed "s/^/graphs=$graphs run=$i  /"
  done
  if [[ -n $nsys ]] && command -v nsys >/dev/null; then
    FORGE_DEVICE=$dev FORGE_GRAPHS=$graphs nsys profile -t cuda,nvtx --cuda-graph-trace=node -o "$nsys-forge-g$graphs" --force-overwrite true \
      "$forge" bench "$model" --codec "$codec" -p "$p" -n "$n" >/dev/null 2>&1
    nsys stats -r cuda_gpu_kern_sum "$nsys-forge-g$graphs.nsys-rep" 2>/dev/null | sed -n '1,30p'
  fi
done
if [[ $nollama == 0 && -n $gguf && -x $llama ]]; then
  echo "== llama.cpp ($llama) model=$gguf"
  "$llama" -m "$gguf" -ngl 99 -p "$p" -n "$n" -r "$r" 2>/dev/null | grep -E '^\| (model|qwen|llama|[a-z0-9])' || true
fi
