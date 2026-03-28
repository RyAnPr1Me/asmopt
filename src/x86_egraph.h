/*
 * x86_egraph.h — x86/x86-64 equality-saturation superoptimizer
 *
 * Provides an API to:
 *   1. Lift a sequence of x86 instructions into an e-graph using SSA-style
 *      register versioning within a straight-line basic block.
 *   2. Run equality saturation (50+ rewrite rules) with CPU-aware costs.
 *   3. Extract cost-optimal expressions using the target CPU's latency/
 *      throughput model and regenerate x86 assembly.
 *
 * Only straight-line code (no branches) within a single basic block is
 * handled.  Instructions that cannot be modelled (complex addressing,
 * SIMD, CALL, RET, PUSH, POP) are passed through unmodified and act as
 * opaque "barriers" — the optimizer flushes its state and restarts.
 */

#ifndef X86_EGRAPH_H
#define X86_EGRAPH_H

#include <stddef.h>

/*
 * Optimize an array of assembly lines and return a newly-allocated array of
 * optimized lines.
 *
 * Parameters:
 *   lines      — input instruction strings (labels, directives, comments and
 *                blank lines are passed through unmodified)
 *   nlines     — number of elements in lines[]
 *   is_att     — 1 for AT&T syntax, 0 for Intel syntax
 *   cpu_name   — target microarchitecture name, e.g. "zen3", "skylake",
 *                "alderlake".  NULL or "generic" uses a conservative model.
 *                The CPU model controls both instruction costs (used during
 *                Souper-style cost-guided extraction) and which CPU-specific
 *                rewrite rules are enabled (e.g. BSF→TZCNT on AMD, LZCNT
 *                over BSR on CPUs with the LZCNT feature).
 *   out_nlines — receives the number of output lines
 *
 * Ownership: caller must free each string and the array itself.
 * Returns NULL on allocation failure.
 */
char **x86_egraph_optimize(const char **lines, size_t nlines,
                            int is_att, const char *cpu_name,
                            size_t *out_nlines);

#endif /* X86_EGRAPH_H */
