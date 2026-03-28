/*
 * cpu_model.h — x86/x86-64 microarchitecture database
 *
 * Provides a per-CPU cost model (latency + throughput) and feature-flag set
 * used by the e-graph superoptimizer to:
 *   - Weight instruction alternatives during cost-based extraction.
 *   - Gate CPU-specific rewrite rules (e.g. BSF→TZCNT on AMD, BMI2 shifts on
 *     Intel).
 *
 * Latencies are reciprocal-throughput values (cycles per instruction when the
 * CPU is fully pipelined) because we optimise for throughput of a sequence, not
 * raw latency of a single instruction.  Latency values come from Agner Fog's
 * instruction tables (https://www.agner.org/optimize/instruction_tables.pdf),
 * Intel and AMD architecture optimisation manuals, and llvm-mca benchmarks.
 *
 * Usage:
 *   const EgCpuModel *m = cpu_model_lookup("zen3");
 *   double cost = cpu_op_cost(m, EG_IMUL);
 *   bool  tzcnt = cpu_has(m, CPU_FEAT_TZCNT);
 */

#ifndef CPU_MODEL_H
#define CPU_MODEL_H

#include "egraph.h"   /* for EgOp */
#include <stdbool.h>
#include <stdint.h>

/* ── Feature flags ────────────────────────────────────────────────────────── */

typedef uint32_t CpuFeatures;

#define CPU_FEAT_NONE       0u
#define CPU_FEAT_TZCNT      (1u << 0)  /* BSF→TZCNT: well-defined on zero  */
#define CPU_FEAT_LZCNT      (1u << 1)  /* BSR→LZCNT (rep lzcnt encoding)   */
#define CPU_FEAT_POPCNT     (1u << 2)  /* POPCNT instruction available      */
#define CPU_FEAT_BMI1       (1u << 3)  /* ANDN, BLSI, BLSMSK, BLSR, …      */
#define CPU_FEAT_BMI2       (1u << 4)  /* SHRX, SHLX, SARX, MULX, RORX    */
#define CPU_FEAT_FAST_IMUL  (1u << 5)  /* 3-cycle IMUL r64, r/m64           */
#define CPU_FEAT_FAST_BSWAP (1u << 6)  /* 1-cycle BSWAP                     */
#define CPU_FEAT_SLOW_SHX   (1u << 7)  /* SHRX/SHLX have higher latency than
                                           plain SHR/SHL on this µarch       */
#define CPU_FEAT_AMD        (1u << 8)  /* AMD-family architecture            */
#define CPU_FEAT_INTEL      (1u << 9)  /* Intel-family architecture          */
#define CPU_FEAT_NO_PARTIAL (1u << 10) /* Avoid partial-register writes      */

/* ── Per-op cost table ───────────────────────────────────────────────────── */

/*
 * Reciprocal throughput in cycles for each EgOp on this µarch.
 * Values < 1.0 indicate that the µarch can sustain more than one per cycle.
 */
typedef struct {
    float ops[EG_OP_COUNT]; /* indexed by EgOp enum value */
} EgOpCosts;

/* ── CPU model ───────────────────────────────────────────────────────────── */

typedef struct EgCpuModel_tag {
    const char  *name;        /* canonical lower-case name, e.g. "zen3"     */
    const char  *family;      /* e.g. "amd-zen", "intel-skylake"            */
    CpuFeatures  features;
    EgOpCosts    costs;
} EgCpuModel;

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * Look up a CPU model by name (case-insensitive).
 * Accepts human-friendly aliases (e.g. "znver3", "core-i9", "skylake-x").
 * Returns the "generic" model if the name is unknown.
 * The returned pointer is to static storage — do not free.
 */
const EgCpuModel *cpu_model_lookup(const char *name);

/*
 * Reciprocal-throughput cost for op on model m.
 * Never returns 0; falls back to a conservative default if the op is unknown.
 */
double cpu_op_cost(const EgCpuModel *m, EgOp op);

/* True iff the CPU model has the given feature flag. */
static inline bool cpu_has(const EgCpuModel *m, CpuFeatures f)
{
    return m && (m->features & f) != 0;
}

/* Return the generic (baseline) CPU model. */
const EgCpuModel *cpu_model_generic(void);

#endif /* CPU_MODEL_H */
