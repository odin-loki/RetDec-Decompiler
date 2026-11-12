#!/usr/bin/env python3
"""Write Forge's golden tiny random Llama as a Hugging Face directory.

Used by tests/neural (retdec-neural-forge-tests) to exercise the backend's
import path: Forge's own generator (third_party/forge/verify/golden/
tiny_llama.py, make_model) writes config.json, model.safetensors and
tokenizer.json; the test then hands the directory to loadModel, which imports
it into a temporary $FORGE_CACHE. Needs numpy and safetensors.

The committed tests/neural/data/tiny_f32.fpk is the same model (seed 1234)
imported with `forge import --codec f32`.

usage: make_tiny_hf.py <forge source dir> <output dir>
"""
import os
import sys


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    forge_dir, out = sys.argv[1], sys.argv[2]
    sys.path.insert(0, os.path.join(forge_dir, "verify", "golden"))
    import tiny_llama  # noqa: E402  (needs numpy; make_model needs safetensors)
    tiny_llama.make_model(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
