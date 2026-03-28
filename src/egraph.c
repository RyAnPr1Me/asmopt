/*
 * egraph.c — E-graph implementation: union-find, hash-consing, rebuild,
 *             cost computation.
 */

#include "egraph.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ───────────────────────────────────────────────────── */

static void *eg_xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "egraph: out of memory\n"); abort(); }
    return p;
}

static void *eg_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q) { fprintf(stderr, "egraph: out of memory\n"); abort(); }
    return q;
}

static char *eg_xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *d = eg_xmalloc(len);
    memcpy(d, s, len);
    return d;
}

/* ── Union-Find ─────────────────────────────────────────────────────────── */

/* Find with full path compression. */
uint32_t eg_find(EGraph *g, uint32_t id)
{
    if (id == EG_NULL) return EG_NULL;
    /* Traverse to root */
    uint32_t root = id;
    while (g->classes[root].parent != root)
        root = g->classes[root].parent;
    /* Path compression */
    while (g->classes[id].parent != root) {
        uint32_t next = g->classes[id].parent;
        g->classes[id].parent = root;
        id = next;
    }
    return root;
}

/* Immediate union (called inside rebuild). Returns new root. */
static uint32_t uf_union(EGraph *g, uint32_t a, uint32_t b)
{
    a = eg_find(g, a);
    b = eg_find(g, b);
    if (a == b) return a;

    /* Union by rank */
    if (g->classes[a].rank < g->classes[b].rank) {
        uint32_t t = a; a = b; b = t;
    }
    /* b's parent becomes a */
    g->classes[b].parent = a;
    if (g->classes[a].rank == g->classes[b].rank)
        g->classes[a].rank++;

    /* Move b's nodes into a */
    for (uint32_t i = 0; i < g->classes[b].nnodes; i++) {
        uint32_t ni = g->classes[b].nodes[i];
        g->nodes[ni].eclass = a;
        /* Append to a's node list */
        if (g->classes[a].nnodes >= g->classes[a].ncap) {
            uint32_t nc = g->classes[a].ncap ? g->classes[a].ncap * 2 : 8;
            g->classes[a].nodes = eg_xrealloc(g->classes[a].nodes,
                                               nc * sizeof(uint32_t));
            g->classes[a].ncap = nc;
        }
        g->classes[a].nodes[g->classes[a].nnodes++] = ni;
    }
    free(g->classes[b].nodes);
    g->classes[b].nodes = NULL;
    g->classes[b].nnodes = 0;
    g->classes[b].ncap  = 0;

    /* Reset cost so it is recomputed */
    g->classes[a].cost = -1.0;
    return a;
}

/* ── Hash table ─────────────────────────────────────────────────────────── */

#define HT_INIT_SIZE 256

static uint32_t ht_hash_node(const EGraph *g, const ENode *n)
{
    uint32_t h = (uint32_t)n->op * 2654435761u;
    for (uint32_t i = 0; i < n->nargs; i++)
        h ^= (eg_find((EGraph *)g, n->args[i]) + i) * 2246822519u;
    if (n->op == EG_CONST) {
        h ^= (uint32_t)(n->imm) * 3266489917u;
        h ^= (uint32_t)(n->imm >> 32) * 668265263u;
    } else if (n->op == EG_VAR && n->name) {
        for (const char *c = n->name; *c; c++)
            h = h * 31 + (unsigned char)*c;
    }
    return h;
}

static bool ht_nodes_equal(EGraph *g, const ENode *a, const ENode *b)
{
    if (a->op != b->op) return false;
    if (a->nargs != b->nargs) return false;
    if (a->op == EG_CONST) return a->imm == b->imm;
    if (a->op == EG_VAR) {
        if (!a->name || !b->name) return a->name == b->name;
        return strcmp(a->name, b->name) == 0;
    }
    for (uint32_t i = 0; i < a->nargs; i++) {
        if (eg_find(g, a->args[i]) != eg_find(g, b->args[i]))
            return false;
    }
    return true;
}

/* Lookup a structurally-described node in the hash table.
   Returns EG_NULL if not found. */
static uint32_t ht_lookup(EGraph *g, const ENode *key)
{
    uint32_t h = ht_hash_node(g, key) & (g->htsize - 1);
    uint32_t ni = g->htable[h];
    while (ni != EG_NULL) {
        if (ht_nodes_equal(g, key, &g->nodes[ni]))
            return ni;
        ni = g->nodes[ni].hnext;
    }
    return EG_NULL;
}

/* Insert node index into the hash table. */
static void ht_insert(EGraph *g, uint32_t ni)
{
    uint32_t h = ht_hash_node(g, &g->nodes[ni]) & (g->htsize - 1);
    g->nodes[ni].hnext = g->htable[h];
    g->htable[h] = ni;
}

/* Rebuild the entire hash table (needed after merges). */
static void ht_rebuild(EGraph *g)
{
    memset(g->htable, 0xff, g->htsize * sizeof(uint32_t)); /* 0xff = EG_NULL bytes */
    for (uint32_t i = 0; i < g->nnodes; i++) {
        /* Canonicalise args */
        ENode *n = &g->nodes[i];
        for (uint32_t j = 0; j < n->nargs; j++)
            n->args[j] = eg_find(g, n->args[j]);
        g->nodes[i].hnext = EG_NULL;
        ht_insert(g, i);
    }
}

/* ── E-Graph lifecycle ──────────────────────────────────────────────────── */

EGraph *eg_create(void)
{
    EGraph *g = eg_xmalloc(sizeof(EGraph));
    memset(g, 0, sizeof(*g));

    g->ncap_nodes   = 64;
    g->nodes        = eg_xmalloc(g->ncap_nodes * sizeof(ENode));

    g->ncap_classes = 64;
    g->classes      = eg_xmalloc(g->ncap_classes * sizeof(EClass));

    g->htsize  = HT_INIT_SIZE;
    g->htable  = eg_xmalloc(g->htsize * sizeof(uint32_t));
    memset(g->htable, 0xff, g->htsize * sizeof(uint32_t));

    g->ncap_pending = 16;
    g->pending_a    = eg_xmalloc(g->ncap_pending * sizeof(uint32_t));
    g->pending_b    = eg_xmalloc(g->ncap_pending * sizeof(uint32_t));

    g->dirty = false;
    return g;
}

void eg_destroy(EGraph *g)
{
    if (!g) return;
    for (uint32_t i = 0; i < g->nnodes; i++)
        free(g->nodes[i].name);
    free(g->nodes);
    for (uint32_t i = 0; i < g->nclasses; i++)
        free(g->classes[i].nodes);
    free(g->classes);
    free(g->htable);
    free(g->pending_a);
    free(g->pending_b);
    free(g);
}

/* ── Allocate a new e-class (returns its ID) ────────────────────────────── */
static uint32_t eg_new_class(EGraph *g)
{
    if (g->nclasses >= g->ncap_classes) {
        g->ncap_classes *= 2;
        g->classes = eg_xrealloc(g->classes,
                                  g->ncap_classes * sizeof(EClass));
    }
    uint32_t id = g->nclasses++;
    EClass *c = &g->classes[id];
    c->parent = id;
    c->rank   = 0;
    c->nodes  = eg_xmalloc(4 * sizeof(uint32_t));
    c->nnodes = 0;
    c->ncap   = 4;
    c->cost   = -1.0;
    c->best   = EG_NULL;
    return id;
}

/* ── Allocate a new e-node (returns its index) ──────────────────────────── */
static uint32_t eg_new_node(EGraph *g)
{
    if (g->nnodes >= g->ncap_nodes) {
        g->ncap_nodes *= 2;
        g->nodes = eg_xrealloc(g->nodes, g->ncap_nodes * sizeof(ENode));
        /* hnext arrays embedded in nodes are fine after realloc */
    }
    uint32_t idx = g->nnodes++;
    memset(&g->nodes[idx], 0, sizeof(ENode));
    g->nodes[idx].hnext  = EG_NULL;
    g->nodes[idx].eclass = EG_NULL;
    return idx;
}

/* ── Add nodes ──────────────────────────────────────────────────────────── */

static uint32_t eg_add_node(EGraph *g, ENode *proto)
{
    /* Canonicalise args first */
    for (uint32_t i = 0; i < proto->nargs; i++)
        proto->args[i] = eg_find(g, proto->args[i]);

    /* Look up in hash table */
    uint32_t existing = ht_lookup(g, proto);
    if (existing != EG_NULL)
        return eg_find(g, g->nodes[existing].eclass);

    /* Allocate new node and class */
    uint32_t ni = eg_new_node(g);
    g->nodes[ni] = *proto;
    /* Duplicate name string (ownership) */
    if (proto->name)
        g->nodes[ni].name = eg_xstrdup(proto->name);

    uint32_t ci = eg_new_class(g);
    g->nodes[ni].eclass = ci;
    g->classes[ci].nodes[g->classes[ci].nnodes++] = ni;

    ht_insert(g, ni);
    return ci;
}

uint32_t eg_add_const(EGraph *g, int64_t value)
{
    ENode proto;
    memset(&proto, 0, sizeof(proto));
    proto.op    = EG_CONST;
    proto.nargs = 0;
    proto.imm   = value;
    proto.hnext = EG_NULL;
    return eg_add_node(g, &proto);
}

uint32_t eg_add_var(EGraph *g, const char *name)
{
    ENode proto;
    memset(&proto, 0, sizeof(proto));
    proto.op    = EG_VAR;
    proto.nargs = 0;
    proto.name  = (char *)name; /* temporarily borrow; eg_add_node dups */
    proto.hnext = EG_NULL;
    return eg_add_node(g, &proto);
}

uint32_t eg_add_op(EGraph *g, EgOp op, const uint32_t *args, uint32_t nargs)
{
    ENode proto;
    memset(&proto, 0, sizeof(proto));
    proto.op    = op;
    proto.nargs = nargs;
    proto.hnext = EG_NULL;
    for (uint32_t i = 0; i < nargs && i < EG_MAX_ARGS; i++)
        proto.args[i] = args[i];
    return eg_add_node(g, &proto);
}

/* ── Merge ──────────────────────────────────────────────────────────────── */

void eg_merge(EGraph *g, uint32_t a, uint32_t b)
{
    a = eg_find(g, a);
    b = eg_find(g, b);
    if (a == b) return;

    if (g->npending >= g->ncap_pending) {
        g->ncap_pending *= 2;
        g->pending_a = eg_xrealloc(g->pending_a,
                                    g->ncap_pending * sizeof(uint32_t));
        g->pending_b = eg_xrealloc(g->pending_b,
                                    g->ncap_pending * sizeof(uint32_t));
    }
    g->pending_a[g->npending] = a;
    g->pending_b[g->npending] = b;
    g->npending++;
    g->dirty = true;
}

/* ── Rebuild ────────────────────────────────────────────────────────────── */

void eg_rebuild(EGraph *g)
{
    if (!g->dirty && g->npending == 0) return;

    /* Process all pending merges */
    for (uint32_t i = 0; i < g->npending; i++)
        uf_union(g, g->pending_a[i], g->pending_b[i]);
    g->npending = 0;

    /* Re-build hash table with canonicalised args; detect duplicates */
    ht_rebuild(g);

    /* Second pass: detect newly-equal e-nodes (same hash bucket, same shape)
       and merge their e-classes.  Repeat until stable. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t h = 0; h < g->htsize; h++) {
            uint32_t i = g->htable[h];
            while (i != EG_NULL) {
                uint32_t j = g->nodes[i].hnext;
                while (j != EG_NULL) {
                    uint32_t jnext = g->nodes[j].hnext;
                    if (ht_nodes_equal(g, &g->nodes[i], &g->nodes[j])) {
                        uint32_t ca = eg_find(g, g->nodes[i].eclass);
                        uint32_t cb = eg_find(g, g->nodes[j].eclass);
                        if (ca != cb) {
                            uf_union(g, ca, cb);
                            changed = true;
                        }
                    }
                    j = jnext;
                }
                i = g->nodes[i].hnext;
            }
        }
        if (changed) ht_rebuild(g);
    }

    g->dirty = false;
}

/* ── Equivalence query ──────────────────────────────────────────────────── */

bool eg_equiv(EGraph *g, uint32_t a, uint32_t b)
{
    return eg_find(g, a) == eg_find(g, b);
}

/* ── Leaf query helpers ─────────────────────────────────────────────────── */

bool eg_is_const(EGraph *g, uint32_t id, int64_t *out_val)
{
    id = eg_find(g, id);
    EClass *c = &g->classes[id];
    for (uint32_t i = 0; i < c->nnodes; i++) {
        ENode *n = &g->nodes[c->nodes[i]];
        if (n->op == EG_CONST) {
            if (out_val) *out_val = n->imm;
            return true;
        }
    }
    return false;
}

bool eg_is_zero(EGraph *g, uint32_t id)
{
    int64_t v;
    return eg_is_const(g, id, &v) && v == 0;
}

bool eg_is_one(EGraph *g, uint32_t id)
{
    int64_t v;
    return eg_is_const(g, id, &v) && v == 1;
}

bool eg_is_minus_one(EGraph *g, uint32_t id)
{
    int64_t v;
    return eg_is_const(g, id, &v) && v == -1;
}

bool eg_is_power_of_two(EGraph *g, uint32_t id, int *out_log2)
{
    int64_t v;
    if (!eg_is_const(g, id, &v)) return false;
    if (v <= 0) return false;
    uint64_t u = (uint64_t)v;
    if ((u & (u - 1)) != 0) return false;
    int log = 0;
    while ((u >>= 1)) log++;
    if (out_log2) *out_log2 = log;
    return true;
}

bool eg_is_var(EGraph *g, uint32_t id, const char **out_name)
{
    id = eg_find(g, id);
    EClass *c = &g->classes[id];
    for (uint32_t i = 0; i < c->nnodes; i++) {
        ENode *n = &g->nodes[c->nodes[i]];
        if (n->op == EG_VAR) {
            if (out_name) *out_name = n->name;
            return true;
        }
    }
    return false;
}

/* ── Cost model ─────────────────────────────────────────────────────────── */

/* Approximate AMD Zen 3 latency-based costs (lower = better). */
double eg_op_cost(EgOp op)
{
    switch (op) {
    case EG_CONST:   return 0.0;
    case EG_VAR:     return 0.0;

    /* Fast integer ALU — 1 cycle latency */
    case EG_ADD:
    case EG_SUB:
    case EG_AND:
    case EG_OR:
    case EG_XOR:
    case EG_NOT:
    case EG_NEG:
    case EG_SHL:
    case EG_SHR:
    case EG_SAR:
    case EG_ROL:
    case EG_ROR:
    case EG_INC:
    case EG_DEC:
        return 1.0;

    /* LEA: 1–3 cycles depending on complexity; use 1.5 as default */
    case EG_LEA:
        return 1.5;

    /* Signed multiply: 3 cycles on Zen 3 */
    case EG_IMUL:
        return 3.0;

    /* Unsigned full multiply: 3–4 cycles */
    case EG_MUL:
        return 4.0;

    /* Division: very expensive */
    case EG_UDIV:
    case EG_SDIV:
    case EG_UMOD:
    case EG_SMOD:
        return 25.0;

    /* Bit-scan / count operations */
    case EG_TZCNT:
    case EG_LZCNT:
        return 2.0;
    case EG_BSF:
    case EG_BSR:
        return 3.0;
    case EG_POPCNT:
        return 2.0;
    case EG_BSWAP:
        return 1.0;

    /* Memory */
    case EG_LOAD:
        return 5.0;   /* L1-hit latency */
    case EG_STORE:
        return 4.0;

    default:
        return 10.0;
    }
}

/* ── Cost computation (bottom-up DP) ────────────────────────────────────── */

/* visited[] prevents re-entrant loops in the e-graph. */
static double compute_cost_rec(EGraph *g, uint32_t id, bool *visited)
{
    id = eg_find(g, id);
    if (g->classes[id].cost >= 0.0)
        return g->classes[id].cost;
    if (visited[id])
        return 1e18; /* cycle — use high sentinel */

    visited[id] = true;

    double best = 1e18;
    uint32_t best_node = EG_NULL;

    EClass *c = &g->classes[id];
    for (uint32_t i = 0; i < c->nnodes; i++) {
        uint32_t ni = c->nodes[i];
        ENode  *n  = &g->nodes[ni];
        double  nc = eg_op_cost(n->op);
        for (uint32_t j = 0; j < n->nargs; j++)
            nc += compute_cost_rec(g, n->args[j], visited);
        if (nc < best) { best = nc; best_node = ni; }
    }

    g->classes[id].cost = best;
    g->classes[id].best = best_node;
    visited[id] = false;
    return best;
}

void eg_compute_costs(EGraph *g)
{
    /* Reset costs */
    for (uint32_t i = 0; i < g->nclasses; i++)
        g->classes[i].cost = -1.0;

    bool *visited = calloc(g->nclasses, sizeof(bool));
    if (!visited) { fprintf(stderr, "egraph: out of memory\n"); abort(); }

    for (uint32_t i = 0; i < g->nclasses; i++) {
        if (eg_find(g, i) == i) /* only roots */
            compute_cost_rec(g, i, visited);
    }
    free(visited);
}
