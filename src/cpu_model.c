/*
 * cpu_model.c — x86/x86-64 microarchitecture cost database
 *
 * Reciprocal-throughput values (cycles) from:
 *   - Agner Fog, "Instruction tables" (Jan 2024 edition)
 *   - AMD Software Optimization Guide for AMD EPYC Processors
 *   - Intel® 64 and IA-32 Architectures Optimization Reference Manual
 *   - llvm-mca measurements on hardware
 *
 * Where a value differs between Agner's table and the vendor's guide, the
 * more conservative (higher) value is used so the optimizer doesn't over-
 * schedule aggressively.
 */

#include "cpu_model.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/*
 * Fill an EgOpCosts table with a common baseline first, then let each model
 * override specific values. This reduces repetition across models.
 */
static void costs_set_generic(EgOpCosts *c)
{
    /* Conservative baseline: assume an in-order micro-controller-like CPU */
    c->ops[EG_CONST]  = 0.0f;
    c->ops[EG_VAR]    = 0.0f;
    c->ops[EG_ADD]    = 1.0f;
    c->ops[EG_SUB]    = 1.0f;
    c->ops[EG_MUL]    = 5.0f;
    c->ops[EG_IMUL]   = 4.0f;
    c->ops[EG_UDIV]   = 30.0f;
    c->ops[EG_SDIV]   = 30.0f;
    c->ops[EG_UMOD]   = 30.0f;
    c->ops[EG_SMOD]   = 30.0f;
    c->ops[EG_AND]    = 1.0f;
    c->ops[EG_OR]     = 1.0f;
    c->ops[EG_XOR]    = 1.0f;
    c->ops[EG_NOT]    = 1.0f;
    c->ops[EG_NEG]    = 1.0f;
    c->ops[EG_SHL]    = 1.0f;
    c->ops[EG_SHR]    = 1.0f;
    c->ops[EG_SAR]    = 1.0f;
    c->ops[EG_ROL]    = 1.0f;
    c->ops[EG_ROR]    = 1.0f;
    c->ops[EG_INC]    = 1.0f;
    c->ops[EG_DEC]    = 1.0f;
    c->ops[EG_BSF]    = 3.0f;
    c->ops[EG_BSR]    = 3.0f;
    c->ops[EG_TZCNT]  = 3.0f;
    c->ops[EG_LZCNT]  = 3.0f;
    c->ops[EG_POPCNT] = 3.0f;
    c->ops[EG_BSWAP]  = 1.0f;
    c->ops[EG_LEA]    = 1.5f;
    c->ops[EG_LOAD]   = 5.0f;
    c->ops[EG_STORE]  = 4.0f;
}

/* ── AMD Zen (Ryzen 1000, EPYC Naples) ─────────────────────────────────── */
/* Source: Agner Fog + AMD Zen Software Optimization Guide rev 3.07        */
static const EgCpuModel MODEL_ZEN = {
    .name    = "zen",
    .family  = "amd-zen",
    .features = CPU_FEAT_TZCNT | CPU_FEAT_LZCNT | CPU_FEAT_POPCNT
              | CPU_FEAT_BMI1 | CPU_FEAT_BMI2 | CPU_FEAT_AMD
              | CPU_FEAT_NO_PARTIAL,
    .costs = { .ops = {
        [EG_CONST]  = 0.0f,
        [EG_VAR]    = 0.0f,
        [EG_ADD]    = 0.25f,  /* 4/cycle throughput */
        [EG_SUB]    = 0.25f,
        [EG_MUL]    = 4.0f,
        [EG_IMUL]   = 3.0f,  /* IMUL r64,r/m64: lat 3, tp 1 */
        [EG_UDIV]   = 35.0f,
        [EG_SDIV]   = 35.0f,
        [EG_UMOD]   = 35.0f,
        [EG_SMOD]   = 35.0f,
        [EG_AND]    = 0.25f,
        [EG_OR]     = 0.25f,
        [EG_XOR]    = 0.25f,
        [EG_NOT]    = 0.25f,
        [EG_NEG]    = 0.25f,
        [EG_SHL]    = 0.5f,
        [EG_SHR]    = 0.5f,
        [EG_SAR]    = 0.5f,
        [EG_ROL]    = 0.5f,
        [EG_ROR]    = 0.5f,
        [EG_INC]    = 0.25f,
        [EG_DEC]    = 0.25f,
        [EG_BSF]    = 3.0f,
        [EG_BSR]    = 3.0f,
        [EG_TZCNT]  = 2.0f,  /* TZCNT: lat 2, reciprocal-tp 0.5 → 2 cycles */
        [EG_LZCNT]  = 2.0f,
        [EG_POPCNT] = 2.0f,  /* lat 2, tp 1 */
        [EG_BSWAP]  = 1.0f,
        [EG_LEA]    = 0.5f,
        [EG_LOAD]   = 4.0f,
        [EG_STORE]  = 2.0f,
    }}
};

/* ── AMD Zen 2 (Ryzen 3000, EPYC Rome) ──────────────────────────────────── */
/* Similar to Zen 1, improved integer throughput.                           */
static const EgCpuModel MODEL_ZEN2 = {
    .name    = "zen2",
    .family  = "amd-zen",
    .features = CPU_FEAT_TZCNT | CPU_FEAT_LZCNT | CPU_FEAT_POPCNT
              | CPU_FEAT_BMI1 | CPU_FEAT_BMI2 | CPU_FEAT_FAST_IMUL
              | CPU_FEAT_AMD | CPU_FEAT_NO_PARTIAL,
    .costs = { .ops = {
        [EG_CONST]  = 0.0f,
        [EG_VAR]    = 0.0f,
        [EG_ADD]    = 0.25f,
        [EG_SUB]    = 0.25f,
        [EG_MUL]    = 4.0f,
        [EG_IMUL]   = 3.0f,
        [EG_UDIV]   = 28.0f,
        [EG_SDIV]   = 28.0f,
        [EG_UMOD]   = 28.0f,
        [EG_SMOD]   = 28.0f,
        [EG_AND]    = 0.25f,
        [EG_OR]     = 0.25f,
        [EG_XOR]    = 0.25f,
        [EG_NOT]    = 0.25f,
        [EG_NEG]    = 0.25f,
        [EG_SHL]    = 0.5f,
        [EG_SHR]    = 0.5f,
        [EG_SAR]    = 0.5f,
        [EG_ROL]    = 0.5f,
        [EG_ROR]    = 0.5f,
        [EG_INC]    = 0.25f,
        [EG_DEC]    = 0.25f,
        [EG_BSF]    = 3.0f,
        [EG_BSR]    = 3.0f,
        [EG_TZCNT]  = 1.0f,  /* Zen 2: tp improved to 1/cycle */
        [EG_LZCNT]  = 1.0f,
        [EG_POPCNT] = 1.0f,
        [EG_BSWAP]  = 1.0f,
        [EG_LEA]    = 0.5f,
        [EG_LOAD]   = 4.0f,
        [EG_STORE]  = 1.0f,
    }}
};

/* ── AMD Zen 3 (Ryzen 5000, EPYC Milan) ─────────────────────────────────── */
/* Source: Agner Fog + AMD PPR for Zen 3 rev B (56713)                     */
static const EgCpuModel MODEL_ZEN3 = {
    .name    = "zen3",
    .family  = "amd-zen",
    .features = CPU_FEAT_TZCNT | CPU_FEAT_LZCNT | CPU_FEAT_POPCNT
              | CPU_FEAT_BMI1 | CPU_FEAT_BMI2 | CPU_FEAT_FAST_IMUL
              | CPU_FEAT_FAST_BSWAP | CPU_FEAT_AMD | CPU_FEAT_NO_PARTIAL,
    .costs = { .ops = {
        [EG_CONST]  = 0.0f,
        [EG_VAR]    = 0.0f,
        [EG_ADD]    = 0.25f,  /* 4-wide integer ALU */
        [EG_SUB]    = 0.25f,
        [EG_MUL]    = 4.0f,
        [EG_IMUL]   = 3.0f,  /* lat 3, tp 1 */
        [EG_UDIV]   = 22.0f,
        [EG_SDIV]   = 22.0f,
        [EG_UMOD]   = 22.0f,
        [EG_SMOD]   = 22.0f,
        [EG_AND]    = 0.25f,
        [EG_OR]     = 0.25f,
        [EG_XOR]    = 0.25f,
        [EG_NOT]    = 0.25f,
        [EG_NEG]    = 0.25f,
        [EG_SHL]    = 0.5f,
        [EG_SHR]    = 0.5f,
        [EG_SAR]    = 0.5f,
        [EG_ROL]    = 0.5f,
        [EG_ROR]    = 0.5f,
        [EG_INC]    = 0.25f,
        [EG_DEC]    = 0.25f,
        [EG_BSF]    = 3.0f,  /* still slower than TZCNT */
        [EG_BSR]    = 3.0f,
        [EG_TZCNT]  = 1.0f,  /* lat 2, tp 1 */
        [EG_LZCNT]  = 1.0f,
        [EG_POPCNT] = 1.0f,
        [EG_BSWAP]  = 1.0f,
        [EG_LEA]    = 0.5f,
        [EG_LOAD]   = 4.0f,  /* L1 hit */
        [EG_STORE]  = 1.0f,
    }}
};

/* ── AMD Zen 4 (Ryzen 7000, EPYC Genoa) ─────────────────────────────────── */
/* Very similar to Zen 3 on integer; AVX-512 added (not modelled here).    */
static const EgCpuModel MODEL_ZEN4 = {
    .name    = "zen4",
    .family  = "amd-zen",
    .features = CPU_FEAT_TZCNT | CPU_FEAT_LZCNT | CPU_FEAT_POPCNT
              | CPU_FEAT_BMI1 | CPU_FEAT_BMI2 | CPU_FEAT_FAST_IMUL
              | CPU_FEAT_FAST_BSWAP | CPU_FEAT_AMD | CPU_FEAT_NO_PARTIAL,
    .costs = { .ops = {
        [EG_CONST]  = 0.0f,
        [EG_VAR]    = 0.0f,
        [EG_ADD]    = 0.25f,
        [EG_SUB]    = 0.25f,
        [EG_MUL]    = 4.0f,
        [EG_IMUL]   = 3.0f,
        [EG_UDIV]   = 20.0f,
        [EG_SDIV]   = 20.0f,
        [EG_UMOD]   = 20.0f,
        [EG_SMOD]   = 20.0f,
        [EG_AND]    = 0.25f,
        [EG_OR]     = 0.25f,
        [EG_XOR]    = 0.25f,
        [EG_NOT]    = 0.25f,
        [EG_NEG]    = 0.25f,
        [EG_SHL]    = 0.5f,
        [EG_SHR]    = 0.5f,
        [EG_SAR]    = 0.5f,
        [EG_ROL]    = 0.5f,
        [EG_ROR]    = 0.5f,
        [EG_INC]    = 0.25f,
        [EG_DEC]    = 0.25f,
        [EG_BSF]    = 3.0f,
        [EG_BSR]    = 3.0f,
        [EG_TZCNT]  = 1.0f,
        [EG_LZCNT]  = 1.0f,
        [EG_POPCNT] = 1.0f,
        [EG_BSWAP]  = 1.0f,
        [EG_LEA]    = 0.5f,
        [EG_LOAD]   = 4.0f,
        [EG_STORE]  = 1.0f,
    }}
};

/* ── Intel Sandy Bridge / Ivy Bridge ────────────────────────────────────── */
/* Source: Agner Fog (sandybridge column).                                  */
static const EgCpuModel MODEL_SANDYBRIDGE = {
    .name    = "sandybridge",
    .family  = "intel-core",
    .features = CPU_FEAT_POPCNT | CPU_FEAT_FAST_IMUL | CPU_FEAT_INTEL,
    .costs = { .ops = {
        [EG_CONST]  = 0.0f,
        [EG_VAR]    = 0.0f,
        [EG_ADD]    = 0.33f,  /* 3/cycle throughput */
        [EG_SUB]    = 0.33f,
        [EG_MUL]    = 5.0f,
        [EG_IMUL]   = 3.0f,  /* lat 3, tp 1 */
        [EG_UDIV]   = 40.0f,
        [EG_SDIV]   = 40.0f,
        [EG_UMOD]   = 40.0f,
        [EG_SMOD]   = 40.0f,
        [EG_AND]    = 0.33f,
        [EG_OR]     = 0.33f,
        [EG_XOR]    = 0.33f,
        [EG_NOT]    = 0.33f,
        [EG_NEG]    = 0.33f,
        [EG_SHL]    = 0.5f,
        [EG_SHR]    = 0.5f,
        [EG_SAR]    = 0.5f,
        [EG_ROL]    = 0.5f,
        [EG_ROR]    = 0.5f,
        [EG_INC]    = 0.33f,
        [EG_DEC]    = 0.33f,
        [EG_BSF]    = 3.0f,
        [EG_BSR]    = 3.0f,
        [EG_TZCNT]  = 3.0f,  /* same port as BSF on snb */
        [EG_LZCNT]  = 3.0f,
        [EG_POPCNT] = 1.0f,
        [EG_BSWAP]  = 1.0f,
        [EG_LEA]    = 0.5f,
        [EG_LOAD]   = 4.0f,
        [EG_STORE]  = 1.0f,
    }}
};

/* ── Intel Haswell / Broadwell ───────────────────────────────────────────── */
/* BMI1/BMI2 added.  Source: Agner Fog (haswell column).                   */
static const EgCpuModel MODEL_HASWELL = {
    .name    = "haswell",
    .family  = "intel-core",
    .features = CPU_FEAT_TZCNT | CPU_FEAT_LZCNT | CPU_FEAT_POPCNT
              | CPU_FEAT_BMI1 | CPU_FEAT_BMI2 | CPU_FEAT_FAST_IMUL
              | CPU_FEAT_INTEL,
    .costs = { .ops = {
        [EG_CONST]  = 0.0f,
        [EG_VAR]    = 0.0f,
        [EG_ADD]    = 0.25f,  /* 4-wide ALU */
        [EG_SUB]    = 0.25f,
        [EG_MUL]    = 4.0f,
        [EG_IMUL]   = 3.0f,
        [EG_UDIV]   = 35.0f,
        [EG_SDIV]   = 35.0f,
        [EG_UMOD]   = 35.0f,
        [EG_SMOD]   = 35.0f,
        [EG_AND]    = 0.25f,
        [EG_OR]     = 0.25f,
        [EG_XOR]    = 0.25f,
        [EG_NOT]    = 0.25f,
        [EG_NEG]    = 0.25f,
        [EG_SHL]    = 0.5f,
        [EG_SHR]    = 0.5f,
        [EG_SAR]    = 0.5f,
        [EG_ROL]    = 0.5f,
        [EG_ROR]    = 0.5f,
        [EG_INC]    = 0.25f,
        [EG_DEC]    = 0.25f,
        [EG_BSF]    = 3.0f,
        [EG_BSR]    = 3.0f,
        [EG_TZCNT]  = 1.0f,  /* TZCNT: lat 3, tp 1 on Haswell */
        [EG_LZCNT]  = 1.0f,
        [EG_POPCNT] = 1.0f,
        [EG_BSWAP]  = 1.0f,
        [EG_LEA]    = 0.5f,
        [EG_LOAD]   = 4.0f,
        [EG_STORE]  = 1.0f,
    }}
};

/* ── Intel Skylake / Skylake-X / Cascade Lake ───────────────────────────── */
/* Source: Agner Fog (skylake column).                                      */
static const EgCpuModel MODEL_SKYLAKE = {
    .name    = "skylake",
    .family  = "intel-skylake",
    .features = CPU_FEAT_TZCNT | CPU_FEAT_LZCNT | CPU_FEAT_POPCNT
              | CPU_FEAT_BMI1 | CPU_FEAT_BMI2 | CPU_FEAT_FAST_IMUL
              | CPU_FEAT_FAST_BSWAP | CPU_FEAT_INTEL | CPU_FEAT_NO_PARTIAL,
    .costs = { .ops = {
        [EG_CONST]  = 0.0f,
        [EG_VAR]    = 0.0f,
        [EG_ADD]    = 0.25f,
        [EG_SUB]    = 0.25f,
        [EG_MUL]    = 4.0f,
        [EG_IMUL]   = 3.0f,
        [EG_UDIV]   = 35.0f,
        [EG_SDIV]   = 35.0f,
        [EG_UMOD]   = 35.0f,
        [EG_SMOD]   = 35.0f,
        [EG_AND]    = 0.25f,
        [EG_OR]     = 0.25f,
        [EG_XOR]    = 0.25f,
        [EG_NOT]    = 0.25f,
        [EG_NEG]    = 0.25f,
        [EG_SHL]    = 0.5f,
        [EG_SHR]    = 0.5f,
        [EG_SAR]    = 0.5f,
        [EG_ROL]    = 0.5f,
        [EG_ROR]    = 0.5f,
        [EG_INC]    = 0.25f,
        [EG_DEC]    = 0.25f,
        [EG_BSF]    = 3.0f,
        [EG_BSR]    = 3.0f,
        [EG_TZCNT]  = 1.0f,
        [EG_LZCNT]  = 1.0f,
        [EG_POPCNT] = 1.0f,
        [EG_BSWAP]  = 1.0f,
        [EG_LEA]    = 0.5f,
        [EG_LOAD]   = 4.0f,
        [EG_STORE]  = 1.0f,
    }}
};

/* ── Intel Ice Lake / Tiger Lake ────────────────────────────────────────── */
/* Larger out-of-order window; slightly better division.                   */
static const EgCpuModel MODEL_ICELAKE = {
    .name    = "icelake",
    .family  = "intel-ice",
    .features = CPU_FEAT_TZCNT | CPU_FEAT_LZCNT | CPU_FEAT_POPCNT
              | CPU_FEAT_BMI1 | CPU_FEAT_BMI2 | CPU_FEAT_FAST_IMUL
              | CPU_FEAT_FAST_BSWAP | CPU_FEAT_INTEL | CPU_FEAT_NO_PARTIAL,
    .costs = { .ops = {
        [EG_CONST]  = 0.0f,
        [EG_VAR]    = 0.0f,
        [EG_ADD]    = 0.25f,
        [EG_SUB]    = 0.25f,
        [EG_MUL]    = 4.0f,
        [EG_IMUL]   = 3.0f,
        [EG_UDIV]   = 28.0f,
        [EG_SDIV]   = 28.0f,
        [EG_UMOD]   = 28.0f,
        [EG_SMOD]   = 28.0f,
        [EG_AND]    = 0.25f,
        [EG_OR]     = 0.25f,
        [EG_XOR]    = 0.25f,
        [EG_NOT]    = 0.25f,
        [EG_NEG]    = 0.25f,
        [EG_SHL]    = 0.5f,
        [EG_SHR]    = 0.5f,
        [EG_SAR]    = 0.5f,
        [EG_ROL]    = 0.5f,
        [EG_ROR]    = 0.5f,
        [EG_INC]    = 0.25f,
        [EG_DEC]    = 0.25f,
        [EG_BSF]    = 3.0f,
        [EG_BSR]    = 3.0f,
        [EG_TZCNT]  = 1.0f,
        [EG_LZCNT]  = 1.0f,
        [EG_POPCNT] = 1.0f,
        [EG_BSWAP]  = 1.0f,
        [EG_LEA]    = 0.5f,
        [EG_LOAD]   = 4.0f,
        [EG_STORE]  = 1.0f,
    }}
};

/* ── Intel Alder Lake / Raptor Lake ─────────────────────────────────────── */
/* Hybrid P+E cores; P-core (Golden Cove) throughputs.                     */
static const EgCpuModel MODEL_ALDERLAKE = {
    .name    = "alderlake",
    .family  = "intel-alder",
    .features = CPU_FEAT_TZCNT | CPU_FEAT_LZCNT | CPU_FEAT_POPCNT
              | CPU_FEAT_BMI1 | CPU_FEAT_BMI2 | CPU_FEAT_FAST_IMUL
              | CPU_FEAT_FAST_BSWAP | CPU_FEAT_INTEL | CPU_FEAT_NO_PARTIAL,
    .costs = { .ops = {
        [EG_CONST]  = 0.0f,
        [EG_VAR]    = 0.0f,
        [EG_ADD]    = 0.2f,   /* 5-wide ALU on P-cores */
        [EG_SUB]    = 0.2f,
        [EG_MUL]    = 4.0f,
        [EG_IMUL]   = 3.0f,
        [EG_UDIV]   = 22.0f,
        [EG_SDIV]   = 22.0f,
        [EG_UMOD]   = 22.0f,
        [EG_SMOD]   = 22.0f,
        [EG_AND]    = 0.2f,
        [EG_OR]     = 0.2f,
        [EG_XOR]    = 0.2f,
        [EG_NOT]    = 0.2f,
        [EG_NEG]    = 0.2f,
        [EG_SHL]    = 0.5f,
        [EG_SHR]    = 0.5f,
        [EG_SAR]    = 0.5f,
        [EG_ROL]    = 0.5f,
        [EG_ROR]    = 0.5f,
        [EG_INC]    = 0.2f,
        [EG_DEC]    = 0.2f,
        [EG_BSF]    = 3.0f,
        [EG_BSR]    = 3.0f,
        [EG_TZCNT]  = 1.0f,
        [EG_LZCNT]  = 1.0f,
        [EG_POPCNT] = 1.0f,
        [EG_BSWAP]  = 1.0f,
        [EG_LEA]    = 0.5f,
        [EG_LOAD]   = 4.0f,
        [EG_STORE]  = 1.0f,
    }}
};

/* ── Intel Sapphire Rapids / Emerald Rapids ─────────────────────────────── */
/* Server class; AVX-512 based (not modelled here, GP integer only).       */
static const EgCpuModel MODEL_SAPPHIRERAPIDS = {
    .name    = "sapphirerapids",
    .family  = "intel-spr",
    .features = CPU_FEAT_TZCNT | CPU_FEAT_LZCNT | CPU_FEAT_POPCNT
              | CPU_FEAT_BMI1 | CPU_FEAT_BMI2 | CPU_FEAT_FAST_IMUL
              | CPU_FEAT_FAST_BSWAP | CPU_FEAT_INTEL | CPU_FEAT_NO_PARTIAL,
    .costs = { .ops = {
        [EG_CONST]  = 0.0f,
        [EG_VAR]    = 0.0f,
        [EG_ADD]    = 0.2f,
        [EG_SUB]    = 0.2f,
        [EG_MUL]    = 4.0f,
        [EG_IMUL]   = 3.0f,
        [EG_UDIV]   = 20.0f,
        [EG_SDIV]   = 20.0f,
        [EG_UMOD]   = 20.0f,
        [EG_SMOD]   = 20.0f,
        [EG_AND]    = 0.2f,
        [EG_OR]     = 0.2f,
        [EG_XOR]    = 0.2f,
        [EG_NOT]    = 0.2f,
        [EG_NEG]    = 0.2f,
        [EG_SHL]    = 0.5f,
        [EG_SHR]    = 0.5f,
        [EG_SAR]    = 0.5f,
        [EG_ROL]    = 0.5f,
        [EG_ROR]    = 0.5f,
        [EG_INC]    = 0.2f,
        [EG_DEC]    = 0.2f,
        [EG_BSF]    = 3.0f,
        [EG_BSR]    = 3.0f,
        [EG_TZCNT]  = 1.0f,
        [EG_LZCNT]  = 1.0f,
        [EG_POPCNT] = 1.0f,
        [EG_BSWAP]  = 1.0f,
        [EG_LEA]    = 0.5f,
        [EG_LOAD]   = 4.0f,
        [EG_STORE]  = 1.0f,
    }}
};

/* ── Generic (conservative baseline) ────────────────────────────────────── */
static EgCpuModel MODEL_GENERIC;  /* filled by cpu_model_generic() */
static bool       MODEL_GENERIC_INIT = false;

const EgCpuModel *cpu_model_generic(void)
{
    if (!MODEL_GENERIC_INIT) {
        MODEL_GENERIC.name    = "generic";
        MODEL_GENERIC.family  = "generic";
        MODEL_GENERIC.features = CPU_FEAT_NONE;
        costs_set_generic(&MODEL_GENERIC.costs);
        MODEL_GENERIC_INIT = true;
    }
    return &MODEL_GENERIC;
}

/* ── Alias / lookup table ────────────────────────────────────────────────── */

typedef struct { const char *alias; const EgCpuModel *model; } AliasEntry;

static const AliasEntry ALIASES[] = {
    /* AMD Zen */
    { "zen",            &MODEL_ZEN   },
    { "zen1",           &MODEL_ZEN   },
    { "znver1",         &MODEL_ZEN   },   /* GCC/Clang -march name */
    { "zen2",           &MODEL_ZEN2  },
    { "znver2",         &MODEL_ZEN2  },
    { "zen3",           &MODEL_ZEN3  },
    { "znver3",         &MODEL_ZEN3  },
    { "zen4",           &MODEL_ZEN4  },
    { "znver4",         &MODEL_ZEN4  },
    /* AMD older (map to Zen as safe approximation) */
    { "k8",             &MODEL_ZEN   },
    { "k8sse3",         &MODEL_ZEN   },
    { "opteron",        &MODEL_ZEN   },
    /* Intel Sandy Bridge / Ivy Bridge */
    { "sandybridge",    &MODEL_SANDYBRIDGE },
    { "ivybridge",      &MODEL_SANDYBRIDGE },
    { "core-avx",       &MODEL_SANDYBRIDGE },
    /* Intel Haswell / Broadwell */
    { "haswell",        &MODEL_HASWELL },
    { "broadwell",      &MODEL_HASWELL },
    { "core-avx2",      &MODEL_HASWELL },
    /* Intel Skylake family */
    { "skylake",        &MODEL_SKYLAKE },
    { "skylake-avx512", &MODEL_SKYLAKE },
    { "cascadelake",    &MODEL_SKYLAKE },
    { "cooperlake",     &MODEL_SKYLAKE },
    { "cannonlake",     &MODEL_SKYLAKE },
    /* Intel Ice Lake / Tiger Lake / Rocket Lake */
    { "icelake",        &MODEL_ICELAKE },
    { "icelake-client", &MODEL_ICELAKE },
    { "icelake-server", &MODEL_ICELAKE },
    { "tigerlake",      &MODEL_ICELAKE },
    { "rocketlake",     &MODEL_ICELAKE },
    /* Intel Alder Lake / Raptor Lake */
    { "alderlake",      &MODEL_ALDERLAKE },
    { "raptorlake",     &MODEL_ALDERLAKE },
    { "meteorlake",     &MODEL_ALDERLAKE },
    /* Intel Sapphire Rapids / Emerald Rapids */
    { "sapphirerapids", &MODEL_SAPPHIRERAPIDS },
    { "emeraldrapids",  &MODEL_SAPPHIRERAPIDS },
    { "graniterapids",  &MODEL_SAPPHIRERAPIDS },
    /* Generic */
    { "generic",        NULL },   /* NULL → use MODEL_GENERIC */
    { "native",         NULL },
    { "x86-64",         NULL },
    { "x86_64",         NULL },
};

#define N_ALIASES (sizeof(ALIASES)/sizeof(ALIASES[0]))

/* Case-insensitive ASCII compare */
static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d) return d;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* Case-insensitive prefix compare (first n chars of a vs all of b) */
static int ci_cmp_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int d = tolower((unsigned char)a[i]) - tolower((unsigned char)b[i]);
        if (d) return d;
    }
    return 0;
}

const EgCpuModel *cpu_model_lookup(const char *name)
{
    if (!name || !*name)
        return cpu_model_generic();

    /* Exact alias match */
    for (size_t i = 0; i < N_ALIASES; i++) {
        if (ci_cmp(name, ALIASES[i].alias) == 0)
            return ALIASES[i].model ? ALIASES[i].model : cpu_model_generic();
    }

    /* Prefix match: "zen3-…", "skylake-…", etc. */
    for (size_t i = 0; i < N_ALIASES; i++) {
        size_t alen = strlen(ALIASES[i].alias);
        const char *n = name;
        size_t nlen = strlen(n);
        if (nlen >= alen &&
            ci_cmp_n(n, ALIASES[i].alias, alen) == 0 &&
            (n[alen] == '-' || n[alen] == '_' || n[alen] == '\0'))
            return ALIASES[i].model ? ALIASES[i].model : cpu_model_generic();
    }

    return cpu_model_generic();
}

double cpu_op_cost(const EgCpuModel *m, EgOp op)
{
    if (!m || (unsigned)op >= EG_OP_COUNT)
        return 1.0;
    float v = m->costs.ops[(unsigned)op];
    return (v > 0.0f) ? (double)v : 1.0;
}
