# Generated provenance summary

Produced by `scripts/ci/generate_provenance.py`. Do not hand-edit.
Scanned `4002` C/C++ files under `src/`, `include/`, `tests/`.

| Class | Count |
|-------|------:|
| `avast-mit` | 1939 |
| `pelib-porst` | 25 |
| `imortek-or-undated` | 2038 |
| `rewrite-tell-leftover` | 0 |

Odin-only files in known-upstream modules: **129** (Imortek additions inside Avast directories, or undated headers).

## Rewrite-tell leftovers (must be zero)

None.

## Odin-only in upstream modules (first 80)

- `src/bin2llvmir/optimizations/cond_branch_opt/cond_branch_opt_ext.cpp`
- `src/bin2llvmir/optimizations/global_const_prop/global_const_prop.cpp`
- `src/bin2llvmir/optimizations/idioms/idioms_ext.cpp`
- `src/bin2llvmir/optimizations/inst_opt/inst_opt_ext.cpp`
- `src/bin2llvmir/optimizations/inst_opt_rda/inst_opt_rda_ext.cpp`
- `src/bin2llvmir/optimizations/jump_table_recovery/jump_table_recovery.cpp`
- `src/bin2llvmir/optimizations/param_return/filter/filter_cc_ext.cpp`
- `src/bin2llvmir/optimizations/phi_to_select/phi_to_select.cpp`
- `src/bin2llvmir/optimizations/redundant_load_store/redundant_load_store.cpp`
- `src/bin2llvmir/optimizations/simple_types/simple_types_fp_ext.cpp`
- `src/bin2llvmir/optimizations/simple_types/simple_types_fp_ext.h`
- `src/bin2llvmir/optimizations/strength_reduction/strength_reduction.cpp`
- `src/bin2llvmir/optimizations/struct_recovery/struct_recovery.cpp`
- `src/bin2llvmir/optimizations/tail_calls/tail_calls.cpp`
- `src/bin2llvmir/optimizations/x87_fpu/x87_fpu_ext.cpp`
- `src/bin2llvmir/utils/ctypes2llvm.cpp`
- `src/capstone2llvmir/arm/arm_thumb_interwork.cpp`
- `src/capstone2llvmir/arm64/arm64_fp_ext.cpp`
- `src/capstone2llvmir/x86/x86_sse.cpp`
- `src/fileformat/file_format/pe/pe_dll_list.cpp`
- `src/fileformat/lattice/format_lattice.cpp`
- `src/fileformat/lief_adapter.cpp`
- `src/fileformat/types/certificate_table/certificate_table.cpp`
- `src/fileformat/types/import_table/elf_import_table.cpp`
- `src/fileformat/utils/ar_archive_format_probe.cpp`
- `src/llvmir2hll/analysis/alias_analysis/alias_analyses/simple_alias_analysis_ext.cpp`
- `src/llvmir2hll/llvm/llvm_intrinsic_converter_ext.cpp`
- `src/llvmir2hll/llvmir2hll.cpp`
- `src/llvmir2hll/optimizer/optimizers/cast_simplifier_optimizer.cpp`
- `src/llvmir2hll/optimizer/optimizers/char_array_to_string_optimizer.cpp`
- `src/llvmir2hll/optimizer/optimizers/copy_propagation_optimizer_ext.cpp`
- `src/llvmir2hll/optimizer/optimizers/dead_local_assign_call_optimizer.cpp`
- `src/llvmir2hll/optimizer/optimizers/goto_cfg_optimizer.cpp`
- `src/llvmir2hll/optimizer/optimizers/if_structure_optimizer_ext.cpp`
- `src/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/bitfield_sub_optimizer.cpp`
- `src/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/mba_sub_optimizer.cpp`
- `src/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/pow2_sub_optimizer.cpp`
- `src/llvmir2hll/optimizer/optimizers/unknown_type_inferrer.cpp`
- `src/llvmir2hll/optimizer/optimizers/while_true_to_for_loop_optimizer_ext.cpp`
- `src/llvmir2hll/var_renamer/var_renamers/debug_var_renamer.cpp`
- `src/pelib/ConfigDirectory.cpp`
- `src/retdec/function_analysis_cache.cpp`
- `src/retdec/llvm_to_ssa.cpp`
- `src/retdec/llvm_to_ssa.h`
- `src/retdec/neural_refine_stub.cpp`
- `src/retdec/semantic_recovery_export.cpp`
- `src/retdec-decompiler/managed_decompiler.cpp`
- `src/retdec-decompiler/managed_decompiler.h`
- `src/retdec-decompiler/output_lang.cpp`
- `src/retdec-decompiler/output_lang.h`
- `src/rtti-finder/vtable/vtable_xref.cpp`
- `src/serdes/semantic_detection.cpp`
- `src/unpackertool/main.cpp`
- `src/utils/crc32.cpp`
- `src/utils/gpu_scanner_cpu.cpp`
- `src/utils/version.cpp`
- `include/retdec/bin2llvmir/optimizations/cond_branch_opt/cond_branch_opt_ext.h`
- `include/retdec/bin2llvmir/optimizations/global_const_prop/global_const_prop.h`
- `include/retdec/bin2llvmir/optimizations/idioms/idioms_ext.h`
- `include/retdec/bin2llvmir/optimizations/inst_opt/inst_opt_ext.h`
- `include/retdec/bin2llvmir/optimizations/inst_opt_rda/inst_opt_rda_ext.h`
- `include/retdec/bin2llvmir/optimizations/jump_table_recovery/jump_table_recovery.h`
- `include/retdec/bin2llvmir/optimizations/phi_to_select/phi_to_select.h`
- `include/retdec/bin2llvmir/optimizations/redundant_load_store/redundant_load_store.h`
- `include/retdec/bin2llvmir/optimizations/simple_types/simple_types_fp_ext.h`
- `include/retdec/bin2llvmir/optimizations/strength_reduction/strength_reduction.h`
- `include/retdec/bin2llvmir/optimizations/struct_recovery/struct_recovery.h`
- `include/retdec/bin2llvmir/optimizations/tail_calls/tail_calls.h`
- `include/retdec/capstone2llvmir/arm/arm_thumb_interwork.h`
- `include/retdec/common/semantic_detection.h`
- `include/retdec/fileformat/lattice/format_lattice.h`
- `include/retdec/fileformat/lattice/format_result.h`
- `include/retdec/fileformat/lief_adapter.h`
- `include/retdec/fileformat/types/certificate_table/certificate.h`
- `include/retdec/fileformat/types/certificate_table/certificate_table.h`
- `include/retdec/fileformat/types/import_table/elf_import_table.h`
- `include/retdec/fileformat/utils/ar_archive_format_probe.h`
- `include/retdec/llvmir2hll/analysis/alias_analysis/alias_analyses/simple_alias_analysis_ext.h`
- `include/retdec/llvmir2hll/llvm/llvm_intrinsic_converter_ext.h`
- `include/retdec/llvmir2hll/llvmir2hll.h`
- … 49 more
