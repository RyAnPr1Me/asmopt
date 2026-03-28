/*
 * x86_egraph.h — x86/x86-64 equality-saturation superoptimizer
 *
 * Provides an API to:
 *   1. Lift a sequence of x86 instructions into an e-graph using SSA-style
 *      register versioning within a straight-line basic block.
 *   2. Run equality saturation (50+ rewrite rules).
 *   3. Extract cost-optimal expressions and regenerate x86 assembly.
 *
 * Only straight-line code (no branches) within a single basic block is
 * handled.  Instructions that cannot be modelled (complex addressing,
 * SIMD, CALL, RET, PUSH, POP) are passed through unmodified and act as
 * opaque "barriers" — the optimizer flushes its state and restarts.
 */

#ifndef X86_EGRAPH_H
#define X86_EGRAPH_H

#include <stddef.h>

/* Optimise a null-terminated array of assembly lines (Intel syntax) and
 * return a newly-allocated null-terminated array of optimized lines.
 *
 * Parameters:
 *   lines        — input instruction strings (may include labels, directives,
 *                  comments and blank lines; those are passed through)
 *   nlines       — number of elements in lines[]
 *   is_att       — 1 for AT&T syntax, 0 for Intel syntax
 *   is_amd       — 1 to enable AMD-specific rewrites (TZCNT over BSF, etc.)
 *   out_nlines   — receives the number of output lines
 *
 * Ownership: caller must free each string and the array itself.
 *
 * Returns NULL on allocation failure.
 */
char **x86_egraph_optimize(const char **lines, size_t nlines,
                             int is_att, int is_amd,
                             size_t *out_nlines);

#endif /* X86_EGRAPH_H */
