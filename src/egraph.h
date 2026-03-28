/*
 * egraph.h — Core e-graph data structure for equality saturation
 *
 * An e-graph (equality graph) compactly represents many equivalent
 * programs simultaneously.  Every "e-class" is a set of mutually
 * equivalent "e-nodes".  Rewrite rules assert new equalities (merge
 * two e-classes); the extraction step then picks the lowest-cost
 * representative expression for every e-class.
 *
 * This implementation follows the rebuild-based approach used by egg
 * (https://egraphs-good.github.io/):
 *   1. add_enode  — structural hashing; returns existing class if found
 *   2. eg_merge   — records a pending merge (does NOT mutate immediately)
 *   3. eg_rebuild — processes pending merges, re-canonicalises enodes
 *   4. apply_rewrites / eg_saturate — outer equality-saturation loop
 *   5. eg_compute_costs / eg_extract — cost-based extraction
 */

#ifndef EGRAPH_H
#define EGRAPH_H

/* Forward declaration so egraph.h does not need to include cpu_model.h.
   cpu_model.h may then include egraph.h for EgOp without a circular dep. */
typedef struct EgCpuModel_tag EgCpuModel;

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Operations ─────────────────────────────────────────────────────────── */

typedef enum {
    /* Leaves */
    EG_CONST  = 0,  /* 64-bit immediate; value in enode.imm              */
    EG_VAR,         /* symbolic variable / register; name in enode.name  */

    /* Arithmetic */
    EG_ADD,
    EG_SUB,
    EG_MUL,         /* unsigned multiply                                  */
    EG_IMUL,        /* signed multiply (x86 imul)                        */
    EG_UDIV,
    EG_SDIV,
    EG_UMOD,
    EG_SMOD,

    /* Bitwise */
    EG_AND,
    EG_OR,
    EG_XOR,
    EG_NOT,         /* unary bitwise-NOT                                  */
    EG_NEG,         /* unary arithmetic negate                            */

    /* Shifts / rotates */
    EG_SHL,
    EG_SHR,         /* logical right shift                                */
    EG_SAR,         /* arithmetic right shift                             */
    EG_ROL,
    EG_ROR,

    /* x86 inc / dec (cheaper encoding than add/sub 1 in many contexts) */
    EG_INC,
    EG_DEC,

    /* Bit-manipulation */
    EG_BSF,
    EG_BSR,
    EG_TZCNT,
    EG_LZCNT,
    EG_POPCNT,
    EG_BSWAP,

    /* Address calculation (LEA):  base + index*scale + disp              */
    EG_LEA,

    /* Memory */
    EG_LOAD,
    EG_STORE,

    /* Sentinel */
    EG_OP_COUNT
} EgOp;

/* ── E-Node ─────────────────────────────────────────────────────────────── */

#define EG_MAX_ARGS 4  /* maximum number of children per e-node */

typedef struct {
    EgOp     op;
    uint32_t args[EG_MAX_ARGS]; /* child e-class IDs                      */
    uint32_t nargs;
    int64_t  imm;               /* for EG_CONST                           */
    char    *name;              /* for EG_VAR (owned, may be NULL)        */
    uint32_t eclass;            /* owning e-class ID                      */
    uint32_t hnext;             /* intrusive hash-chain (EG_NULL = end)   */
} ENode;

/* ── E-Class ────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t  parent;  /* union-find parent; self ↔ root                  */
    uint32_t  rank;    /* union-find rank                                  */
    uint32_t *nodes;   /* indices into graph->nodes[]                     */
    uint32_t  nnodes;
    uint32_t  ncap;
    double    cost;    /* extraction cost; <0 means not yet computed       */
    uint32_t  best;    /* index of best e-node for extraction              */
} EClass;

/* ── E-Graph ────────────────────────────────────────────────────────────── */

typedef struct {
    /* Node pool */
    ENode   *nodes;
    uint32_t nnodes;
    uint32_t ncap_nodes;

    /* Class pool */
    EClass  *classes;
    uint32_t nclasses;
    uint32_t ncap_classes;

    /* Structural-sharing hash table (separate-chaining, power-of-2 size) */
    uint32_t *htable;   /* bucket heads (EG_NULL = empty bucket)          */
    uint32_t  htsize;   /* must be a power of 2                           */

    /* Deferred merges (processed during rebuild) */
    uint32_t *pending_a;
    uint32_t *pending_b;
    uint32_t  npending;
    uint32_t  ncap_pending;

    bool dirty; /* true after a merge, before the next rebuild            */
} EGraph;

/* Sentinel "no class" value */
#define EG_NULL UINT32_MAX

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Lifecycle */
EGraph  *eg_create(void);
void     eg_destroy(EGraph *g);

/* Add leaf nodes (returns existing class if structurally identical) */
uint32_t eg_add_const(EGraph *g, int64_t value);
uint32_t eg_add_var(EGraph *g, const char *name);

/* Add an internal node; args[0..nargs-1] are child e-class IDs.
   Returns the e-class that contains the new (or existing) node. */
uint32_t eg_add_op(EGraph *g, EgOp op, const uint32_t *args, uint32_t nargs);

/* Assert that two e-classes are equivalent (deferred until rebuild). */
void     eg_merge(EGraph *g, uint32_t a, uint32_t b);

/* Find canonical (root) e-class ID with path compression. */
uint32_t eg_find(EGraph *g, uint32_t id);

/* Process all pending merges and re-canonicalise the hash table. */
void     eg_rebuild(EGraph *g);

/* True iff a and b are in the same e-class. */
bool     eg_equiv(EGraph *g, uint32_t a, uint32_t b);

/* Query helpers */
bool     eg_is_const(EGraph *g, uint32_t id, int64_t *out_val);
bool     eg_is_zero(EGraph *g, uint32_t id);
bool     eg_is_one(EGraph *g, uint32_t id);
bool     eg_is_minus_one(EGraph *g, uint32_t id);
bool     eg_is_power_of_two(EGraph *g, uint32_t id, int *out_log2);
bool     eg_is_var(EGraph *g, uint32_t id, const char **out_name);

/* Cost-based extraction: sets class->cost and class->best for every class.
   Must be called after the final eg_rebuild().
   m: optional CPU model; if NULL the generic model is used. */
void     eg_compute_costs(EGraph *g, const struct EgCpuModel_tag *m);

/* Return the base instruction cost for op on model m (generic if m==NULL). */
double   eg_op_cost(const struct EgCpuModel_tag *m, EgOp op);

#endif /* EGRAPH_H */
