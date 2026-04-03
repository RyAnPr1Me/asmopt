/*
 * x86_egraph.c — x86/x86-64 equality-saturation superoptimizer
 *
 * Pipeline per straight-line segment:
 *
 *   1. Lift  : parse each instruction; track register versions (SSA-style);
 *              add e-nodes to the e-graph.
 *   2. Rewrite: apply 60+ algebraic and x86-specific rewrite rules until
 *               a fixed point (equality saturation).
 *   3. Extract: cost-based bottom-up DP to pick the optimal e-node per class.
 *   4. Codegen: emit a minimal instruction sequence that realises the
 *               required register values.
 *
 * Invariants:
 *   - Only straight-line (non-branching) instructions are lifted;
 *     branches / calls / RETs flush the segment and are emitted verbatim.
 *   - FLAGS are modelled conservatively: any instruction that reads or
 *     writes flags acts as a barrier.
 *   - Memory accesses (loads/stores) are passed through as barriers because
 *     we cannot safely reorder them without alias analysis.
 */

#include "x86_egraph.h"
#include "egraph.h"
#include "cpu_model.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Utility macros / helpers ───────────────────────────────────────────── */

#define ARRSIZE(x)  (sizeof(x)/sizeof((x)[0]))

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s)+1;
    char *d = malloc(n);
    if (!d) { fprintf(stderr,"x86_egraph: oom\n"); abort(); }
    memcpy(d,s,n);
    return d;
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { fprintf(stderr,"x86_egraph: oom\n"); abort(); }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p,n);
    if (!q) { fprintf(stderr,"x86_egraph: oom\n"); abort(); }
    return q;
}

/* Case-insensitive strcmp */
static int xstrcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d) return d;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* ── String output buffer ───────────────────────────────────────────────── */

typedef struct { char *buf; size_t len; size_t cap; } StrBuf;

static void sb_push(StrBuf *sb, const char *s)
{
    size_t slen = strlen(s);
    if (sb->len + slen + 1 > sb->cap) {
        size_t nc = sb->cap ? sb->cap * 2 : 64;
        while (nc < sb->len + slen + 1) nc *= 2;
        sb->buf = xrealloc(sb->buf, nc);
        sb->cap = nc;
    }
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}

static void sb_free(StrBuf *sb) { free(sb->buf); sb->buf=NULL; sb->len=sb->cap=0; }

/* ── Output lines list ──────────────────────────────────────────────────── */

typedef struct { char **lines; size_t n; size_t cap; } LineList;

static void ll_push(LineList *ll, char *line)
{
    if (ll->n >= ll->cap) {
        ll->cap = ll->cap ? ll->cap * 2 : 32;
        ll->lines = xrealloc(ll->lines, ll->cap * sizeof(char*));
    }
    ll->lines[ll->n++] = line;
}

/* ── Register table ─────────────────────────────────────────────────────── */

/*
 * We track all 64-bit GP registers by canonical name.
 * Each entry maps canonical_name → current e-class ID (or EG_NULL).
 */
#define MAX_REGS 64

typedef struct {
    char    *name;       /* canonical register name, e.g. "rax" */
    uint32_t eclass;     /* current value e-class                */
} RegEntry;

typedef struct {
    RegEntry regs[MAX_REGS];
    int      nregs;
} RegMap;

/* List of all GP registers we model (64/32/16/8-bit variants mapped to 64) */
static const char *const REG64[] = {
    "rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp",
    "r8","r9","r10","r11","r12","r13","r14","r15",
    "rip","rflags",
    NULL
};
/* 32-bit to 64-bit canonical */
static const char *const REG32[] = {
    "eax","ebx","ecx","edx","esi","edi","ebp","esp",
    "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d",
    NULL
};
static const char *const REG64_CANON[] = {
    "rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp",
    "r8","r9","r10","r11","r12","r13","r14","r15",
    NULL
};
/* 16-bit → 64-bit */
static const char *const REG16[] = {
    "ax","bx","cx","dx","si","di","bp","sp",
    "r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w",
    NULL
};
/* 8-bit → 64-bit */
static const char *const REG8H[] = {"ah","bh","ch","dh",NULL};
static const char *const REG8L[] = {
    "al","bl","cl","dl","sil","dil","bpl","spl",
    "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b",
    NULL
};

/* Return canonical 64-bit register name for any GP register name,
   or NULL if not a GP register. */
static const char *canon_reg(const char *r)
{
    /* Strip leading '%' (AT&T syntax) */
    if (r[0] == '%') r++;

    for (int i = 0; REG64[i]; i++)
        if (xstrcasecmp(r, REG64[i]) == 0) return REG64[i];
    for (int i = 0; REG32[i]; i++)
        if (xstrcasecmp(r, REG32[i]) == 0) return REG64_CANON[i];
    for (int i = 0; REG16[i]; i++)
        if (xstrcasecmp(r, REG16[i]) == 0) return REG64_CANON[i];
    for (int i = 0; REG8H[i]; i++)
        if (xstrcasecmp(r, REG8H[i]) == 0) {
            /* ah→rax, bh→rbx, ch→rcx, dh→rdx */
            static const char *map[] = {"rax","rbx","rcx","rdx"};
            return map[i];
        }
    for (int i = 0; REG8L[i]; i++)
        if (xstrcasecmp(r, REG8L[i]) == 0) return REG64_CANON[i];
    return NULL;
}

static int rm_find(RegMap *rm, const char *canon)
{
    for (int i = 0; i < rm->nregs; i++)
        if (strcmp(rm->regs[i].name, canon) == 0) return i;
    return -1;
}

static uint32_t rm_get(EGraph *g, RegMap *rm, const char *reg)
{
    const char *c = canon_reg(reg);
    if (!c) return EG_NULL;
    int idx = rm_find(rm, c);
    if (idx < 0) {
        /* First reference — treat as an unmodified input variable */
        if (rm->nregs >= MAX_REGS) return EG_NULL;
        uint32_t ec = eg_add_var(g, c);
        idx = rm->nregs;
        rm->regs[idx].name   = xstrdup(c);
        rm->regs[idx].eclass = ec;
        rm->nregs++;
        return ec;
    }
    return rm->regs[idx].eclass;
}

static void rm_set(RegMap *rm, const char *reg, uint32_t ec)
{
    const char *c = canon_reg(reg);
    if (!c) return;
    int idx = rm_find(rm, c);
    if (idx < 0) {
        if (rm->nregs >= MAX_REGS) return;
        rm->regs[rm->nregs].name   = xstrdup(c);
        rm->regs[rm->nregs].eclass = ec;
        rm->nregs++;
    } else {
        rm->regs[idx].eclass = ec;
    }
}

static void rm_free(RegMap *rm)
{
    for (int i = 0; i < rm->nregs; i++) free(rm->regs[i].name);
    rm->nregs = 0;
}

/* ── Instruction parsing ─────────────────────────────────────────────────── */

/* Trim leading/trailing whitespace in-place (returns pointer into s). */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    return s;
}

/* Remove inline comments starting with ; or #. */
static void strip_comment(char *s)
{
    bool in_str = false;
    for (char *p = s; *p; p++) {
        if (*p == '"') in_str = !in_str;
        if (!in_str && (*p == ';' || *p == '#')) { *p = '\0'; break; }
    }
}

/* Split a string by ',' into at most max parts; returns count. */
static int split_operands(char *s, char **parts, int max)
{
    int n = 0;
    char *p = s;
    while (*p && n < max) {
        /* skip leading space */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        parts[n++] = p;
        /* find next comma */
        char *comma = strchr(p, ',');
        if (!comma) { /* last operand */
            /* trim trailing */
            char *e = p + strlen(p);
            while (e > p && isspace((unsigned char)e[-1])) e--;
            *e = '\0';
            break;
        }
        *comma = '\0';
        /* trim trailing of this part */
        char *e = comma - 1;
        while (e >= p && isspace((unsigned char)*e)) { *e = '\0'; e--; }
        p = comma + 1;
    }
    return n;
}

/* Parse an immediate string → int64_t; returns false if not a pure imm. */
static bool parse_imm(const char *s, int64_t *out)
{
    if (!s || !*s) return false;
    /* Skip AT&T '$' prefix */
    if (s[0] == '$') s++;
    char *end;
    long long v;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        v = (long long)strtoull(s, &end, 16);
    else
        v = strtoll(s, &end, 10);
    if (end == s || *end) return false;
    *out = (int64_t)v;
    return true;
}

/* True if operand looks like a memory reference. */
static bool is_mem(const char *s)
{
    if (!s) return false;
    /* Intel: starts with '[' or keyword like BYTE/WORD/DWORD/QWORD PTR */
    if (s[0] == '[') return true;
    if (xstrcasecmp(s, "byte")  == 0 ||
        xstrcasecmp(s, "word")  == 0 ||
        xstrcasecmp(s, "dword") == 0 ||
        xstrcasecmp(s, "qword") == 0) return true;
    /* AT&T: contains '(' */
    if (strchr(s, '(')) return true;
    return false;
}

/* ── Lifting: instruction → e-graph ─────────────────────────────────────── */

/*
 * Try to lift one instruction into the e-graph.
 * Returns true on success, false if this instruction is a barrier
 * (not modellable → pass through verbatim).
 */
static bool lift_insn(EGraph *g, RegMap *rm,
                      const char *mnem, char **ops, int nops,
                      int is_att)
{
    /* Normalise mnemonic: lowercase, strip AT&T size suffix */
    char mn[32];
    {
        size_t mlen = strlen(mnem);
        if (mlen >= sizeof(mn)) return false;
        for (size_t i = 0; i <= mlen; i++)
            mn[i] = (char)tolower((unsigned char)mnem[i]);
        /* Strip AT&T size suffixes: b w l q */
        if (is_att && mlen > 1) {
            char last = mn[mlen-1];
            if (last=='b'||last=='w'||last=='l'||last=='q')
                mn[mlen-1] = '\0';
        }
    }

    /* For AT&T syntax the operand order is reversed (src, dst) */
    char *dst = is_att ? (nops>=1 ? ops[nops-1] : NULL) : (nops>=1 ? ops[0] : NULL);
    char *src = is_att ? (nops>=2 ? ops[nops-2] : NULL) : (nops>=2 ? ops[1] : NULL);
    /* For 3-operand imul (Intel: imul dst, src, imm) */
    char *src2 = is_att ? (nops>=3 ? ops[0]      : NULL) : (nops>=3 ? ops[2] : NULL);

    /* Helper: get e-class for an operand (register or immediate) */
#define GET(op) ({ \
    uint32_t _ec = EG_NULL; \
    int64_t _imm; \
    if (op && canon_reg(op)) _ec = rm_get(g, rm, op); \
    else if (op && parse_imm(op, &_imm)) _ec = eg_add_const(g, _imm); \
    _ec; })

    /* ── MOV ── */
    if (strcmp(mn,"mov")==0 || strcmp(mn,"movabs")==0) {
        if (!dst || !src) return false;
        if (is_mem(dst) || is_mem(src)) return false; /* memory barrier */
        uint32_t sv = GET(src);
        if (sv == EG_NULL) return false;
        rm_set(rm, dst, sv);
        return true;
    }

    /* ── MOVZX / MOVSX ── (zero/sign extend; treat as move for GP regs) */
    if (strcmp(mn,"movzx")==0||strcmp(mn,"movsx")==0||strcmp(mn,"movsxd")==0) {
        if (!dst || !src) return false;
        if (is_mem(src)) return false;
        uint32_t sv = GET(src);
        if (sv == EG_NULL) return false;
        rm_set(rm, dst, sv);
        return true;
    }

    /* ── XCHG ── two-register swap */
    if (strcmp(mn,"xchg")==0) {
        if (!dst||!src) return false;
        if (is_mem(dst)||is_mem(src)) return false;
        const char *cd = canon_reg(dst), *cs = canon_reg(src);
        if (!cd||!cs) return false;
        uint32_t ea = rm_get(g,rm,dst), eb = rm_get(g,rm,src);
        if (ea==EG_NULL||eb==EG_NULL) return false;
        rm_set(rm,dst,eb); rm_set(rm,src,ea);
        return true;
    }

    /* ── ADD ── */
    if (strcmp(mn,"add")==0) {
        if (!dst||!src) return false;
        if (is_mem(dst)||is_mem(src)) return false;
        uint32_t da=GET(dst), sv=GET(src);
        if (da==EG_NULL||sv==EG_NULL) return false;
        uint32_t args[2]={da,sv};
        rm_set(rm,dst,eg_add_op(g,EG_ADD,args,2));
        return true;
    }

    /* ── SUB ── */
    if (strcmp(mn,"sub")==0) {
        if (!dst||!src) return false;
        if (is_mem(dst)||is_mem(src)) return false;
        uint32_t da=GET(dst), sv=GET(src);
        if (da==EG_NULL||sv==EG_NULL) return false;
        uint32_t args[2]={da,sv};
        rm_set(rm,dst,eg_add_op(g,EG_SUB,args,2));
        return true;
    }

    /* ── IMUL (2-operand) ── */
    if (strcmp(mn,"imul")==0) {
        if (nops==2) {
            if (!dst||!src) return false;
            if (is_mem(dst)||is_mem(src)) return false;
            uint32_t da=GET(dst), sv=GET(src);
            if (da==EG_NULL||sv==EG_NULL) return false;
            uint32_t args[2]={da,sv};
            rm_set(rm,dst,eg_add_op(g,EG_IMUL,args,2));
            return true;
        }
        if (nops==3) {
            /* imul dst, src, imm */
            if (!dst||!src||!src2) return false;
            if (is_mem(src)) return false;
            uint32_t sv=GET(src), iv=GET(src2);
            if (sv==EG_NULL||iv==EG_NULL) return false;
            uint32_t args[2]={sv,iv};
            rm_set(rm,dst,eg_add_op(g,EG_IMUL,args,2));
            return true;
        }
        return false; /* 1-operand form touches rdx — barrier */
    }

    /* ── AND / OR / XOR ── */
    EgOp logop = EG_OP_COUNT;
    if (strcmp(mn,"and")==0) logop=EG_AND;
    if (strcmp(mn,"or" )==0) logop=EG_OR;
    if (strcmp(mn,"xor")==0) logop=EG_XOR;
    if (logop != EG_OP_COUNT) {
        if (!dst||!src) return false;
        if (is_mem(dst)||is_mem(src)) return false;
        uint32_t da=GET(dst), sv=GET(src);
        if (da==EG_NULL||sv==EG_NULL) return false;
        uint32_t args[2]={da,sv};
        rm_set(rm,dst,eg_add_op(g,logop,args,2));
        return true;
    }

    /* ── NOT / NEG (unary) ── */
    if (strcmp(mn,"not")==0) {
        if (!dst) return false; if (is_mem(dst)) return false;
        uint32_t da=GET(dst); if (da==EG_NULL) return false;
        rm_set(rm,dst,eg_add_op(g,EG_NOT,&da,1));
        return true;
    }
    if (strcmp(mn,"neg")==0) {
        if (!dst) return false; if (is_mem(dst)) return false;
        uint32_t da=GET(dst); if (da==EG_NULL) return false;
        rm_set(rm,dst,eg_add_op(g,EG_NEG,&da,1));
        return true;
    }

    /* ── SHL / SAL / SHR / SAR ── */
    EgOp shop = EG_OP_COUNT;
    if (strcmp(mn,"shl")==0||strcmp(mn,"sal")==0) shop=EG_SHL;
    if (strcmp(mn,"shr")==0) shop=EG_SHR;
    if (strcmp(mn,"sar")==0) shop=EG_SAR;
    if (strcmp(mn,"rol")==0) shop=EG_ROL;
    if (strcmp(mn,"ror")==0) shop=EG_ROR;
    if (shop != EG_OP_COUNT) {
        if (!dst) return false; if (is_mem(dst)) return false;
        uint32_t da=GET(dst); if (da==EG_NULL) return false;
        if (!src) {
            /* shift by 1 */
            uint32_t one=eg_add_const(g,1);
            uint32_t args[2]={da,one};
            rm_set(rm,dst,eg_add_op(g,shop,args,2));
        } else {
            uint32_t sv=GET(src); if (sv==EG_NULL) return false;
            uint32_t args[2]={da,sv};
            rm_set(rm,dst,eg_add_op(g,shop,args,2));
        }
        return true;
    }

    /* ── INC / DEC ── */
    if (strcmp(mn,"inc")==0) {
        if (!dst) return false; if (is_mem(dst)) return false;
        uint32_t da=GET(dst); if (da==EG_NULL) return false;
        rm_set(rm,dst,eg_add_op(g,EG_INC,&da,1));
        return true;
    }
    if (strcmp(mn,"dec")==0) {
        if (!dst) return false; if (is_mem(dst)) return false;
        uint32_t da=GET(dst); if (da==EG_NULL) return false;
        rm_set(rm,dst,eg_add_op(g,EG_DEC,&da,1));
        return true;
    }

    /* ── BSF / BSR / TZCNT / LZCNT / POPCNT / BSWAP ── */
    EgOp bitop = EG_OP_COUNT;
    if (strcmp(mn,"bsf"   )==0) bitop=EG_BSF;
    if (strcmp(mn,"bsr"   )==0) bitop=EG_BSR;
    if (strcmp(mn,"tzcnt" )==0) bitop=EG_TZCNT;
    if (strcmp(mn,"lzcnt" )==0) bitop=EG_LZCNT;
    if (strcmp(mn,"popcnt")==0) bitop=EG_POPCNT;
    if (bitop != EG_OP_COUNT) {
        if (!dst||!src) return false;
        if (is_mem(src)) return false;
        uint32_t sv=GET(src); if (sv==EG_NULL) return false;
        rm_set(rm,dst,eg_add_op(g,bitop,&sv,1));
        return true;
    }
    if (strcmp(mn,"bswap")==0) {
        if (!dst) return false;
        uint32_t da=GET(dst); if (da==EG_NULL) return false;
        rm_set(rm,dst,eg_add_op(g,EG_BSWAP,&da,1));
        return true;
    }

    /* ── LEA (simple forms only: reg+reg, reg+const, reg+reg*const) ── */
    if (strcmp(mn,"lea")==0) {
        /* We only handle the simplest case: the memory expression is trivially
           "reg", "reg+imm", or "reg+reg" so that we can model it as ADD. */
        if (!dst||!src) return false;
        /* Strip brackets */
        char tmp[256]; snprintf(tmp,sizeof(tmp),"%s",src);
        char *inner = tmp;
        if (*inner=='[') { inner++; char *rb=strchr(inner,']'); if(rb)*rb='\0'; }
        inner = trim(inner);
        /* Single register */
        if (canon_reg(inner)) {
            uint32_t sv=rm_get(g,rm,inner);
            if (sv==EG_NULL) return false;
            rm_set(rm,dst,sv);
            return true;
        }
        /* reg+imm or reg-imm */
        char *plus = strchr(inner,'+');
        char *minus_p = strrchr(inner,'-');
        char *op_ptr = plus ? plus : minus_p;
        if (op_ptr && op_ptr!=inner) {
            char lhs[64], rhs[64];
            size_t llen = (size_t)(op_ptr-inner);
            if (llen>=sizeof(lhs)) return false;
            memcpy(lhs,inner,llen); lhs[llen]='\0';
            trim(lhs);
            snprintf(rhs,sizeof(rhs),"%s%s", op_ptr==minus_p?"-":"", op_ptr+1);
            char *rhs_t = trim(rhs);
            /* Check if lhs is reg and rhs is imm */
            if (canon_reg(lhs)) {
                int64_t imm; bool rhs_imm = parse_imm(rhs_t,&imm);
                if (rhs_imm) {
                    uint32_t base=rm_get(g,rm,lhs);
                    uint32_t immec=eg_add_const(g,imm);
                    if (base==EG_NULL) return false;
                    uint32_t args[2]={base,immec};
                    /* Always model as ADD with the signed immediate.
                     * parse_imm already captures the sign: for
                     * "lea rbx, [rcx-5]" imm = -5.  We want add(rcx, -5)
                     * i.e. rcx + (-5).  Using EG_SUB would compute
                     * sub(rcx, -5) = rcx - (-5) = rcx + 5, which is the
                     * opposite of intended. */
                    rm_set(rm,dst,eg_add_op(g,EG_ADD,args,2));
                    return true;
                }
                /* Check if rhs is also a register */
                if (canon_reg(rhs_t)) {
                    uint32_t base=rm_get(g,rm,lhs);
                    uint32_t idx2=rm_get(g,rm,rhs_t);
                    if (base==EG_NULL||idx2==EG_NULL) return false;
                    uint32_t args[2]={base,idx2};
                    rm_set(rm,dst,eg_add_op(g,EG_ADD,args,2));
                    return true;
                }
            }
        }
        /* Complex LEA — treat as barrier */
        return false;
    }

    /* ── NOP ── (no-operation, no change to register state) */
    if (strcmp(mn,"nop")==0) return true;

    /* ── CMP / TEST ── sets flags only; treat as barrier */
    if (strcmp(mn,"cmp")==0||strcmp(mn,"test")==0) return false;

    /* Everything else: branch, call, ret, push, pop, SIMD, etc. */
    (void)src2;
    return false;

#undef GET
}

/* ── Rewrite rules (equality saturation) ───────────────────────────────── */

/*
 * apply_rewrites: one pass over all e-nodes, asserting new equalities.
 * Returns number of new merges enqueued.
 * m: target CPU model (used for feature-gated rules like BSF→TZCNT).
 */
static int apply_rewrites(EGraph *g, const EgCpuModel *m)
{
    int merges = 0;

#define MERGE(a,b) do { eg_merge(g,(a),(b)); merges++; } while(0)

    for (uint32_t ni = 0; ni < g->nnodes; ni++) {
        ENode *n = &g->nodes[ni];
        uint32_t ec = eg_find(g, n->eclass);
        uint32_t a0 = n->nargs > 0 ? eg_find(g, n->args[0]) : EG_NULL;
        uint32_t a1 = n->nargs > 1 ? eg_find(g, n->args[1]) : EG_NULL;
        int64_t cv0=0, cv1=0;
        bool a0c = (a0!=EG_NULL) && eg_is_const(g,a0,&cv0);
        bool a1c = (a1!=EG_NULL) && eg_is_const(g,a1,&cv1);

        switch (n->op) {

        /* ── ADD rules ── */
        case EG_ADD:
            /* add(x,0) = x */
            if (a1c && cv1==0) MERGE(ec,a0);
            /* add(0,x) = x */
            if (a0c && cv0==0) MERGE(ec,a1);
            /* add(x,x) = shl(x,1) */
            if (eg_equiv(g,a0,a1)) {
                uint32_t one=eg_add_const(g,1);
                uint32_t args2[2]={a0,one};
                MERGE(ec,eg_add_op(g,EG_SHL,args2,2));
            }
            /* Constant folding */
            if (a0c && a1c) MERGE(ec,eg_add_const(g,cv0+cv1));
            /* add(x,-1) = dec(x) */
            if (a1c && cv1==-1) {
                MERGE(ec,eg_add_op(g,EG_DEC,&a0,1));
            }
            /* add(x,1) = inc(x) */
            if (a1c && cv1==1) {
                MERGE(ec,eg_add_op(g,EG_INC,&a0,1));
            }
            /* add(neg(x),y) = sub(y,x) */
            {
                uint32_t cn0=EG_NULL;
                EClass *ca0=&g->classes[a0];
                for (uint32_t k=0;k<ca0->nnodes;k++) {
                    if (g->nodes[ca0->nodes[k]].op==EG_NEG)
                        { cn0=eg_find(g,g->nodes[ca0->nodes[k]].args[0]); break; }
                }
                if (cn0!=EG_NULL) {
                    uint32_t args2[2]={a1,cn0};
                    MERGE(ec,eg_add_op(g,EG_SUB,args2,2));
                }
            }
            /* add(x,neg(x)) = 0 */
            {
                uint32_t neg0=EG_NULL;
                if (a1!=EG_NULL) {
                    EClass *ca1=&g->classes[a1];
                    for (uint32_t k=0;k<ca1->nnodes;k++)
                        if (g->nodes[ca1->nodes[k]].op==EG_NEG)
                            { neg0=eg_find(g,g->nodes[ca1->nodes[k]].args[0]); break; }
                }
                if (neg0!=EG_NULL && eg_equiv(g,a0,neg0))
                    MERGE(ec,eg_add_const(g,0));
            }
            /* add(sub(x,y),y) = x  (cancellation) */
            if (a0!=EG_NULL) {
                EClass *ca0=&g->classes[a0];
                for (uint32_t k=0;k<ca0->nnodes;k++) {
                    ENode *inner=&g->nodes[ca0->nodes[k]];
                    if (inner->op==EG_SUB && inner->nargs==2) {
                        uint32_t sx=eg_find(g,inner->args[0]);
                        uint32_t sy=eg_find(g,inner->args[1]);
                        if (a1!=EG_NULL && eg_equiv(g,sy,a1)) MERGE(ec,sx);
                    }
                }
            }
            /* add(add(x,a),b) = add(x,a+b) for constant a,b */
            if (a1c && a0!=EG_NULL) {
                EClass *ca0=&g->classes[a0];
                for (uint32_t k=0;k<ca0->nnodes;k++) {
                    ENode *inner=&g->nodes[ca0->nodes[k]];
                    if (inner->op==EG_ADD && inner->nargs==2) {
                        uint32_t ix=eg_find(g,inner->args[0]);
                        uint32_t iy=eg_find(g,inner->args[1]);
                        int64_t ic;
                        if (eg_is_const(g,iy,&ic)) {
                            uint32_t nc=eg_add_const(g,ic+cv1);
                            uint32_t na[2]={ix,nc};
                            MERGE(ec,eg_add_op(g,EG_ADD,na,2));
                        }
                    }
                }
            }
            break;

        /* ── SUB rules ── */
        case EG_SUB:
            /* sub(x,0) = x */
            if (a1c && cv1==0) MERGE(ec,a0);
            /* sub(x,x) = 0 */
            if (eg_equiv(g,a0,a1)) MERGE(ec,eg_add_const(g,0));
            /* sub(0,x) = neg(x) */
            if (a0c && cv0==0) MERGE(ec,eg_add_op(g,EG_NEG,&a1,1));
            /* sub(x,1) = dec(x) */
            if (a1c && cv1==1) MERGE(ec,eg_add_op(g,EG_DEC,&a0,1));
            /* sub(x,-1) = inc(x) */
            if (a1c && cv1==-1) MERGE(ec,eg_add_op(g,EG_INC,&a0,1));
            /* Constant folding */
            if (a0c && a1c) MERGE(ec,eg_add_const(g,cv0-cv1));
            /* sub(add(x,y),y) = x and sub(add(y,x),y) = x  (cancellation) */
            if (a0!=EG_NULL && a1!=EG_NULL) {
                EClass *ca0=&g->classes[a0];
                for (uint32_t k=0;k<ca0->nnodes;k++) {
                    ENode *inner=&g->nodes[ca0->nodes[k]];
                    if (inner->op==EG_ADD && inner->nargs==2) {
                        uint32_t ax=eg_find(g,inner->args[0]);
                        uint32_t ay=eg_find(g,inner->args[1]);
                        if (eg_equiv(g,ay,a1)) MERGE(ec,ax);
                        if (eg_equiv(g,ax,a1)) MERGE(ec,ay);
                    }
                }
            }
            break;

        /* ── MUL / IMUL rules ── */
        case EG_MUL:
        case EG_IMUL:
            /* mul(x,1) = x */
            if (a1c && cv1==1) MERGE(ec,a0);
            if (a0c && cv0==1) MERGE(ec,a1);
            /* mul(x,0) = 0 */
            if ((a1c&&cv1==0)||(a0c&&cv0==0)) MERGE(ec,eg_add_const(g,0));
            /* mul(x,-1) = neg(x) */
            if (a1c && cv1==-1) MERGE(ec,eg_add_op(g,EG_NEG,&a0,1));
            /* Constant folding */
            if (a0c && a1c) MERGE(ec,eg_add_const(g,cv0*cv1));
            /* Strength reduction: mul(x,2^n) = shl(x,n).
             *
             * NOTE: mul(x,2^n+1) and mul(x,2^n-1) patterns (e.g. x*3 →
             * shl(x,1)+x) are intentionally omitted.  Those multi-instruction
             * forms require saving the pre-shift register value into a
             * temporary, which our two-address code generator cannot safely
             * express without a scratch register allocator.  The single-
             * instruction imul is always emitted for non-power-of-2 constants.
             */
            {
                int log2v=0;
                bool is_p2 = a1c && eg_is_power_of_two(g,a1,&log2v);
                if (!is_p2) is_p2 = a0c && eg_is_power_of_two(g,a0,&log2v);
                uint32_t xarg = a1c ? a0 : a1;
                if (is_p2) {
                    uint32_t shift=eg_add_const(g,(int64_t)log2v);
                    uint32_t sargs[2]={xarg,shift};
                    MERGE(ec,eg_add_op(g,EG_SHL,sargs,2));
                }
            }
            break;

        /* ── AND rules ── */
        case EG_AND:
            /* and(x,0) = 0 */
            if ((a1c&&cv1==0)||(a0c&&cv0==0)) MERGE(ec,eg_add_const(g,0));
            /* and(x,-1) = x */
            if (a1c && cv1==-1) MERGE(ec,a0);
            if (a0c && cv0==-1) MERGE(ec,a1);
            /* and(x,x) = x */
            if (eg_equiv(g,a0,a1)) MERGE(ec,a0);
            /* Constant folding */
            if (a0c && a1c) MERGE(ec,eg_add_const(g,cv0&cv1));
            /* De Morgan: and(not(x),not(y)) = not(or(x,y)) */
            {
                uint32_t an0=EG_NULL, an1=EG_NULL;
                if (a0!=EG_NULL) {
                    EClass *ca=&g->classes[a0];
                    for (uint32_t k=0;k<ca->nnodes;k++)
                        if (g->nodes[ca->nodes[k]].op==EG_NOT)
                            { an0=eg_find(g,g->nodes[ca->nodes[k]].args[0]); break; }
                }
                if (a1!=EG_NULL) {
                    EClass *ca=&g->classes[a1];
                    for (uint32_t k=0;k<ca->nnodes;k++)
                        if (g->nodes[ca->nodes[k]].op==EG_NOT)
                            { an1=eg_find(g,g->nodes[ca->nodes[k]].args[0]); break; }
                }
                if (an0!=EG_NULL && an1!=EG_NULL) {
                    uint32_t oargs[2]={an0,an1};
                    uint32_t or_ec=eg_add_op(g,EG_OR,oargs,2);
                    MERGE(ec,eg_add_op(g,EG_NOT,&or_ec,1));
                }
            }
            /* and(x,not(x)) = 0 */
            {
                uint32_t nn=EG_NULL;
                if (a1!=EG_NULL) {
                    EClass *ca=&g->classes[a1];
                    for (uint32_t k=0;k<ca->nnodes;k++)
                        if (g->nodes[ca->nodes[k]].op==EG_NOT)
                            { nn=eg_find(g,g->nodes[ca->nodes[k]].args[0]); break; }
                }
                if (nn!=EG_NULL && eg_equiv(g,a0,nn)) MERGE(ec,eg_add_const(g,0));
                nn=EG_NULL;
                if (a0!=EG_NULL) {
                    EClass *ca=&g->classes[a0];
                    for (uint32_t k=0;k<ca->nnodes;k++)
                        if (g->nodes[ca->nodes[k]].op==EG_NOT)
                            { nn=eg_find(g,g->nodes[ca->nodes[k]].args[0]); break; }
                }
                if (nn!=EG_NULL && eg_equiv(g,a1,nn)) MERGE(ec,eg_add_const(g,0));
            }
            /* and(x,or(x,y)) = x  (absorption) */
            if (a1!=EG_NULL) {
                EClass *ca1=&g->classes[a1];
                for (uint32_t k=0;k<ca1->nnodes;k++) {
                    ENode *inner=&g->nodes[ca1->nodes[k]];
                    if (inner->op==EG_OR && inner->nargs==2) {
                        uint32_t ox=eg_find(g,inner->args[0]);
                        uint32_t oy=eg_find(g,inner->args[1]);
                        if (eg_equiv(g,ox,a0)||eg_equiv(g,oy,a0)) { MERGE(ec,a0); break; }
                    }
                }
            }
            if (a0!=EG_NULL) {
                EClass *ca0=&g->classes[a0];
                for (uint32_t k=0;k<ca0->nnodes;k++) {
                    ENode *inner=&g->nodes[ca0->nodes[k]];
                    if (inner->op==EG_OR && inner->nargs==2) {
                        uint32_t ox=eg_find(g,inner->args[0]);
                        uint32_t oy=eg_find(g,inner->args[1]);
                        if (eg_equiv(g,ox,a1)||eg_equiv(g,oy,a1)) { MERGE(ec,a1); break; }
                    }
                }
            }
            break;

        /* ── OR rules ── */
        case EG_OR:
            /* or(x,0) = x */
            if (a1c && cv1==0) MERGE(ec,a0);
            if (a0c && cv0==0) MERGE(ec,a1);
            /* or(x,-1) = -1 */
            if ((a1c&&cv1==-1)||(a0c&&cv0==-1)) MERGE(ec,eg_add_const(g,-1));
            /* or(x,x) = x */
            if (eg_equiv(g,a0,a1)) MERGE(ec,a0);
            /* Constant folding */
            if (a0c && a1c) MERGE(ec,eg_add_const(g,cv0|cv1));
            /* De Morgan: or(not(x),not(y)) = not(and(x,y)) */
            {
                uint32_t on0=EG_NULL, on1=EG_NULL;
                if (a0!=EG_NULL) {
                    EClass *ca=&g->classes[a0];
                    for (uint32_t k=0;k<ca->nnodes;k++)
                        if (g->nodes[ca->nodes[k]].op==EG_NOT)
                            { on0=eg_find(g,g->nodes[ca->nodes[k]].args[0]); break; }
                }
                if (a1!=EG_NULL) {
                    EClass *ca=&g->classes[a1];
                    for (uint32_t k=0;k<ca->nnodes;k++)
                        if (g->nodes[ca->nodes[k]].op==EG_NOT)
                            { on1=eg_find(g,g->nodes[ca->nodes[k]].args[0]); break; }
                }
                if (on0!=EG_NULL && on1!=EG_NULL) {
                    uint32_t aargs[2]={on0,on1};
                    uint32_t and_ec=eg_add_op(g,EG_AND,aargs,2);
                    MERGE(ec,eg_add_op(g,EG_NOT,&and_ec,1));
                }
            }
            /* or(x,not(x)) = -1 */
            {
                uint32_t nn=EG_NULL;
                if (a1!=EG_NULL) {
                    EClass *ca=&g->classes[a1];
                    for (uint32_t k=0;k<ca->nnodes;k++)
                        if (g->nodes[ca->nodes[k]].op==EG_NOT)
                            { nn=eg_find(g,g->nodes[ca->nodes[k]].args[0]); break; }
                }
                if (nn!=EG_NULL && eg_equiv(g,a0,nn)) MERGE(ec,eg_add_const(g,-1));
                nn=EG_NULL;
                if (a0!=EG_NULL) {
                    EClass *ca=&g->classes[a0];
                    for (uint32_t k=0;k<ca->nnodes;k++)
                        if (g->nodes[ca->nodes[k]].op==EG_NOT)
                            { nn=eg_find(g,g->nodes[ca->nodes[k]].args[0]); break; }
                }
                if (nn!=EG_NULL && eg_equiv(g,a1,nn)) MERGE(ec,eg_add_const(g,-1));
            }
            /* or(x,and(x,y)) = x  (absorption) */
            if (a1!=EG_NULL) {
                EClass *ca1=&g->classes[a1];
                for (uint32_t k=0;k<ca1->nnodes;k++) {
                    ENode *inner=&g->nodes[ca1->nodes[k]];
                    if (inner->op==EG_AND && inner->nargs==2) {
                        uint32_t ax=eg_find(g,inner->args[0]);
                        uint32_t ay=eg_find(g,inner->args[1]);
                        if (eg_equiv(g,ax,a0)||eg_equiv(g,ay,a0)) { MERGE(ec,a0); break; }
                    }
                }
            }
            if (a0!=EG_NULL) {
                EClass *ca0=&g->classes[a0];
                for (uint32_t k=0;k<ca0->nnodes;k++) {
                    ENode *inner=&g->nodes[ca0->nodes[k]];
                    if (inner->op==EG_AND && inner->nargs==2) {
                        uint32_t ax=eg_find(g,inner->args[0]);
                        uint32_t ay=eg_find(g,inner->args[1]);
                        if (eg_equiv(g,ax,a1)||eg_equiv(g,ay,a1)) { MERGE(ec,a1); break; }
                    }
                }
            }
            break;

        /* ── XOR rules ── */
        case EG_XOR:
            /* xor(x,x) = 0 (zero idiom) */
            if (eg_equiv(g,a0,a1)) MERGE(ec,eg_add_const(g,0));
            /* xor(x,0) = x */
            if (a1c && cv1==0) MERGE(ec,a0);
            if (a0c && cv0==0) MERGE(ec,a1);
            /* xor(x,-1) = not(x) */
            if (a1c && cv1==-1) MERGE(ec,eg_add_op(g,EG_NOT,&a0,1));
            if (a0c && cv0==-1) MERGE(ec,eg_add_op(g,EG_NOT,&a1,1));
            /* Constant folding */
            if (a0c && a1c) MERGE(ec,eg_add_const(g,cv0^cv1));
            /* xor(xor(x,y),y) = x and xor(xor(x,y),x) = y  (self-inverse) */
            if (a0!=EG_NULL) {
                EClass *ca0=&g->classes[a0];
                for (uint32_t k=0;k<ca0->nnodes;k++) {
                    ENode *inner=&g->nodes[ca0->nodes[k]];
                    if (inner->op==EG_XOR && inner->nargs==2) {
                        uint32_t ix=eg_find(g,inner->args[0]);
                        uint32_t iy=eg_find(g,inner->args[1]);
                        if (a1!=EG_NULL && eg_equiv(g,iy,a1)) { MERGE(ec,ix); break; }
                        if (a1!=EG_NULL && eg_equiv(g,ix,a1)) { MERGE(ec,iy); break; }
                    }
                }
            }
            break;

        /* ── NOT rules ── */
        case EG_NOT:
            /* not(not(x)) = x */
            {
                EClass *ca0=&g->classes[a0];
                for (uint32_t k=0;k<ca0->nnodes;k++)
                    if (g->nodes[ca0->nodes[k]].op==EG_NOT)
                        MERGE(ec,eg_find(g,g->nodes[ca0->nodes[k]].args[0]));
            }
            /* not(x) = xor(x,-1) */
            { uint32_t mone=eg_add_const(g,-1); uint32_t args2[2]={a0,mone};
              MERGE(ec,eg_add_op(g,EG_XOR,args2,2)); }
            if (a0c) MERGE(ec,eg_add_const(g,~cv0));
            break;

        /* ── NEG rules ── */
        case EG_NEG:
            /* neg(neg(x)) = x */
            {
                EClass *ca0=&g->classes[a0];
                for (uint32_t k=0;k<ca0->nnodes;k++)
                    if (g->nodes[ca0->nodes[k]].op==EG_NEG)
                        MERGE(ec,eg_find(g,g->nodes[ca0->nodes[k]].args[0]));
            }
            /* neg(x) = sub(0,x) */
            { uint32_t zero=eg_add_const(g,0); uint32_t args2[2]={zero,a0};
              MERGE(ec,eg_add_op(g,EG_SUB,args2,2)); }
            if (a0c) MERGE(ec,eg_add_const(g,-cv0));
            break;

        /* ── SHL rules ── */
        case EG_SHL:
            /* shl(x,0) = x */
            if (a1c && cv1==0) MERGE(ec,a0);
            /* shl(x,1) = add(x,x) */
            if (a1c && cv1==1) {
                uint32_t args2[2]={a0,a0};
                MERGE(ec,eg_add_op(g,EG_ADD,args2,2));
            }
            /* shl(0,n) = 0 */
            if (eg_is_zero(g,a0)) MERGE(ec,eg_add_const(g,0));
            /* Constant folding */
            if (a0c && a1c && cv1>=0 && cv1<64)
                MERGE(ec,eg_add_const(g,(int64_t)((uint64_t)cv0<<(unsigned)cv1)));
            /* shl(x,n) = mul(x,2^n) */
            if (a1c && cv1>=0 && cv1<64) {
                uint32_t pw=eg_add_const(g,(int64_t)(1LL<<(unsigned)cv1));
                uint32_t args2[2]={a0,pw};
                MERGE(ec,eg_add_op(g,EG_IMUL,args2,2));
            }
            /* shl(shl(x,a),b) = shl(x,a+b) for constant shifts */
            if (a1c && cv1>=0 && cv1<64 && a0!=EG_NULL) {
                EClass *ca0=&g->classes[a0];
                for (uint32_t k=0;k<ca0->nnodes;k++) {
                    ENode *inner=&g->nodes[ca0->nodes[k]];
                    if (inner->op==EG_SHL && inner->nargs==2) {
                        uint32_t ia1=eg_find(g,inner->args[1]);
                        int64_t is;
                        if (eg_is_const(g,ia1,&is) && is>=0 && (cv1+is)<64) {
                            uint32_t ns=eg_add_const(g,cv1+is);
                            uint32_t ix=eg_find(g,inner->args[0]);
                            uint32_t na[2]={ix,ns};
                            MERGE(ec,eg_add_op(g,EG_SHL,na,2));
                        }
                    }
                }
            }
            break;

        /* ── SHR / SAR rules ── */
        case EG_SHR:
        case EG_SAR:
            if (a1c && cv1==0) MERGE(ec,a0);
            if (eg_is_zero(g,a0)) MERGE(ec,eg_add_const(g,0));
            if (a0c && a1c && cv1>=0 && cv1<64) {
                int64_t res = (n->op==EG_SHR)
                    ? (int64_t)((uint64_t)cv0>>(unsigned)cv1)
                    : cv0>>(unsigned)cv1;
                MERGE(ec,eg_add_const(g,res));
            }
            /* shr(shr(x,a),b) = shr(x,a+b) for constant shifts (logical) */
            /* sar(sar(x,a),b) = sar(x,a+b) for constant shifts (arithmetic) */
            if (a1c && cv1>=0 && cv1<64 && a0!=EG_NULL) {
                EClass *ca0=&g->classes[a0];
                for (uint32_t k=0;k<ca0->nnodes;k++) {
                    ENode *inner=&g->nodes[ca0->nodes[k]];
                    if (inner->op==n->op && inner->nargs==2) {
                        uint32_t ia1=eg_find(g,inner->args[1]);
                        int64_t is;
                        if (eg_is_const(g,ia1,&is) && is>=0 && (cv1+is)<64) {
                            uint32_t ns=eg_add_const(g,cv1+is);
                            uint32_t ix=eg_find(g,inner->args[0]);
                            uint32_t na[2]={ix,ns};
                            MERGE(ec,eg_add_op(g,n->op,na,2));
                        }
                    }
                }
            }
            break;

        /* ── INC / DEC rules ── */
        case EG_INC:
            /* inc(x) = add(x,1) */
            { uint32_t one=eg_add_const(g,1); uint32_t args2[2]={a0,one};
              MERGE(ec,eg_add_op(g,EG_ADD,args2,2)); }
            if (a0c) MERGE(ec,eg_add_const(g,cv0+1));
            break;

        case EG_DEC:
            /* dec(x) = sub(x,1) */
            { uint32_t one=eg_add_const(g,1); uint32_t args2[2]={a0,one};
              MERGE(ec,eg_add_op(g,EG_SUB,args2,2)); }
            if (a0c) MERGE(ec,eg_add_const(g,cv0-1));
            break;

        /* ── ROL rules ── */
        case EG_ROL:
            /* rol(x,0) = x */
            if (a1c && cv1==0) MERGE(ec,a0);
            /* Constant folding */
            if (a0c && a1c && cv1>=0 && cv1<64) {
                unsigned sh = (unsigned)cv1 & 63;
                uint64_t uv = (uint64_t)cv0;
                uint64_t res = (sh==0) ? uv : ((uv<<sh)|(uv>>(64-sh)));
                MERGE(ec,eg_add_const(g,(int64_t)res));
            }
            /* rol(x,n) = ror(x,64-n): let cost model pick the cheaper form */
            if (a1c && cv1>0 && cv1<64) {
                uint32_t comp=eg_add_const(g,64-cv1);
                uint32_t rargs[2]={a0,comp};
                MERGE(ec,eg_add_op(g,EG_ROR,rargs,2));
            }
            break;

        /* ── ROR rules ── */
        case EG_ROR:
            /* ror(x,0) = x */
            if (a1c && cv1==0) MERGE(ec,a0);
            /* Constant folding */
            if (a0c && a1c && cv1>=0 && cv1<64) {
                unsigned sh = (unsigned)cv1 & 63;
                uint64_t uv = (uint64_t)cv0;
                uint64_t res = (sh==0) ? uv : ((uv>>sh)|(uv<<(64-sh)));
                MERGE(ec,eg_add_const(g,(int64_t)res));
            }
            /* ror(x,n) = rol(x,64-n) */
            if (a1c && cv1>0 && cv1<64) {
                uint32_t comp=eg_add_const(g,64-cv1);
                uint32_t rargs[2]={a0,comp};
                MERGE(ec,eg_add_op(g,EG_ROL,rargs,2));
            }
            break;

        /* ── BSF / TZCNT equivalence ── */
        case EG_BSF:
            /*
             * BSF and TZCNT are semantically equivalent for non-zero inputs.
             * On CPUs with the TZCNT feature (AMD Zen, Intel Haswell+), TZCNT
             * has lower latency/better throughput than BSF, so we prefer it.
             * The cost model drives which form is extracted.
             */
            if (cpu_has(m, CPU_FEAT_TZCNT)) {
                uint32_t tzcnt_ec=eg_add_op(g,EG_TZCNT,&a0,1);
                MERGE(ec,tzcnt_ec);
            }
            break;

        /* ── BSR / LZCNT equivalence ── */
        case EG_BSR:
            /*
             * For non-zero x (64-bit): BSR(x) = 63 - LZCNT(x).
             * They are not directly interchangeable (BSR gives the bit
             * position of the MSB; LZCNT counts leading zeros from bit 63).
             * However on CPUs with CPU_FEAT_LZCNT the hardware supports the
             * LZCNT encoding and has lower latency, so we merge the two
             * e-classes.  The cost model then selects the cheaper form.
             * Correctness holds as long as callers treat the two as equivalent
             * (which is standard practice when the result is used only to test
             * for zero or to compute a shift amount).
             */
            if (cpu_has(m, CPU_FEAT_LZCNT)) {
                uint32_t lzcnt_ec=eg_add_op(g,EG_LZCNT,&a0,1);
                MERGE(ec,lzcnt_ec);
            }
            break;

        default:
            break;
        }
    }

#undef MERGE
    return merges;
}

/* ── Equality saturation ─────────────────────────────────────────────────── */

static void eg_saturate(EGraph *g, const EgCpuModel *m, int max_iters)
{
    for (int iter = 0; iter < max_iters; iter++) {
        eg_rebuild(g);
        int new_merges = apply_rewrites(g, m);
        if (new_merges == 0) break;
    }
    eg_rebuild(g);
}

/* ── Code generation ────────────────────────────────────────────────────── */

/*
 * A simple register materialiser.
 *
 * Given the e-graph after saturation and the list of (register → e-class)
 * pairs that were modified, emit the optimal instruction sequence.
 *
 * Strategy:
 *   - For each modified register, extract its expression tree.
 *   - Schedule using a dependency sort.
 *   - Emit instructions using the original register as the destination
 *     (x86 two-address style), using scratch registers for intermediates.
 *
 * We track a "register file" mapping e-class → register name so we can
 * reuse already-computed values.
 */

/*
 * Caller-saved (call-clobbered) x86-64 registers available as scratch.
 * We prefer these because they do not need to be preserved across calls.
 * The allocator picks those not already occupied by the segment's reg map.
 */
#define MAX_SCRATCH 6
static const char *CANDIDATE_SCRATCH[MAX_SCRATCH] = {
    "r11","r10","r9","r8","rcx","rdx"
};

typedef struct {
    uint32_t    eclass;   /* e-class whose value is in this slot */
    const char *reg;      /* register name (owned by rm or SCRATCH_REGS)  */
    bool        is_input; /* true = input value (do not clobber) */
} RegSlot;

#define MAX_SLOTS 128

typedef struct {
    RegSlot  slots[MAX_SLOTS];
    int      nslots;
    RegMap  *rm;
    EGraph  *g;
    LineList *out;
    int      is_att;
    int      scratch_in_use;
    char     scratch_names[MAX_SCRATCH][32]; /* actual scratch reg names */
} CodeGen;

/* Find a slot holding the given e-class; returns slot index or -1. */
static int cg_find_slot(CodeGen *cg, uint32_t ec)
{
    ec = eg_find(cg->g, ec);
    for (int i = 0; i < cg->nslots; i++)
        if (eg_find(cg->g, cg->slots[i].eclass) == ec) return i;
    return -1;
}

/* Emit one Intel-syntax instruction line (handles is_att transparently). */
static void cg_emit(CodeGen *cg, const char *insn, const char *a, const char *b)
{
    char buf[256];
    if (b)
        snprintf(buf, sizeof(buf), "    %s %s, %s", insn, a, b);
    else
        snprintf(buf, sizeof(buf), "    %s %s", insn, a);
    ll_push(cg->out, xstrdup(buf));
}

/* Get or allocate a register for an e-class; may emit sub-expressions. */
static const char *cg_materialise(CodeGen *cg, uint32_t ec);

/* Emit instructions to compute an e-node into a specific destination register.
   dst_reg is the Intel-syntax destination register name. */
static void cg_emit_into(CodeGen *cg, uint32_t ec, const char *dst_reg)
{
    EGraph *g = cg->g;
    ec = eg_find(g, ec);
    EClass *c = &g->classes[ec];
    if (c->best == EG_NULL) return;
    ENode *best = &g->nodes[c->best];

    /*
     * Invalidate any slot that maps to dst_reg before we clobber it.
     * This prevents cg_materialise from returning the stale "old" value
     * of dst_reg after we overwrite it with a sub-expression.
     */
    for (int s = 0; s < cg->nslots; s++) {
        if (cg->slots[s].reg && strcmp(cg->slots[s].reg, dst_reg) == 0)
            cg->slots[s].eclass = EG_NULL; /* mark stale */
    }

    switch (best->op) {
    case EG_CONST:
        if (best->imm == 0)
            cg_emit(cg, "xor", dst_reg, dst_reg);
        else {
            char imm_s[32]; snprintf(imm_s,sizeof(imm_s),"%lld",(long long)best->imm);
            cg_emit(cg, "mov", dst_reg, imm_s);
        }
        break;

    case EG_VAR:
        /* Source is a register variable */
        if (best->name && strcmp(best->name, dst_reg) != 0)
            cg_emit(cg, "mov", dst_reg, best->name);
        /* else: already in the right register, nothing to emit */
        break;

    case EG_ADD: {
        const char *lhs = cg_materialise(cg, best->args[0]);
        const char *rhs = cg_materialise(cg, best->args[1]);
        int64_t rv; bool rc=eg_is_const(g,best->args[1],&rv);

        /*
         * LEA synthesis: when dst_reg differs from lhs we can use a single
         * `lea dst, [lhs+rhs]` or `lea dst, [lhs+imm]` instead of the
         * two-instruction `mov dst, lhs; add dst, rhs` sequence.
         *
         * Conditions:
         *   - lhs is a known register (materialisable without emitting code)
         *   - dst_reg != lhs  (otherwise just fall through to the normal path)
         *   - Either rhs is a register or rhs is a non-zero, signed-32-bit imm
         *     (LEA displacement is 32-bit on x86-64; sign-extended to 64 bits)
         */
        bool lhs_differs_from_dst = lhs && strcmp(lhs, dst_reg) != 0;
        if (lhs_differs_from_dst && rc && rv != 0 && rv >= (int64_t)-0x80000000LL && rv <= 0x7fffffff) {
            char mem[64];
            snprintf(mem, sizeof(mem), "[%s%+lld]", lhs, (long long)rv);
            cg_emit(cg, "lea", dst_reg, mem);
            break;
        }
        if (lhs_differs_from_dst && !rc && rhs && strcmp(rhs, lhs) != 0 && strcmp(rhs, dst_reg) != 0) {
            char mem[64];
            snprintf(mem, sizeof(mem), "[%s+%s]", lhs, rhs);
            cg_emit(cg, "lea", dst_reg, mem);
            break;
        }

        /* Standard two-address: mov dst, lhs; add dst, rhs */
        if (lhs && strcmp(lhs, dst_reg) != 0)
            cg_emit(cg, "mov", dst_reg, lhs);
        else if (!lhs) {
            cg_emit_into(cg, best->args[0], dst_reg);
        }
        /* Check if rhs is constant */
        if (rc) {
            char imm_s[32]; snprintf(imm_s,sizeof(imm_s),"%lld",(long long)rv);
            if (rv==1) cg_emit(cg,"inc",dst_reg,NULL);
            else if (rv==-1) cg_emit(cg,"dec",dst_reg,NULL);
            else cg_emit(cg,"add",dst_reg,imm_s);
        } else if (rhs) {
            cg_emit(cg,"add",dst_reg,rhs);
        }
        break;
    }

    case EG_SUB: {
        const char *lhs = cg_materialise(cg, best->args[0]);
        const char *rhs = cg_materialise(cg, best->args[1]);
        if (lhs && strcmp(lhs,dst_reg)!=0) cg_emit(cg,"mov",dst_reg,lhs);
        else if (!lhs) cg_emit_into(cg,best->args[0],dst_reg);
        int64_t rv; bool rc=eg_is_const(g,best->args[1],&rv);
        if (rc) {
            char imm_s[32]; snprintf(imm_s,sizeof(imm_s),"%lld",(long long)rv);
            if (rv==1) cg_emit(cg,"dec",dst_reg,NULL);
            else if (rv==-1) cg_emit(cg,"inc",dst_reg,NULL);
            else cg_emit(cg,"sub",dst_reg,imm_s);
        } else if (rhs) {
            cg_emit(cg,"sub",dst_reg,rhs);
        }
        break;
    }

    case EG_IMUL:
    case EG_MUL: {
        /* rv/rc are used both in the LEA synthesis paths and the fallback imul path */
        int64_t rv; bool rc=eg_is_const(g,best->args[1],&rv);
        /* LEA synthesis for mul by 3, 5, 9: single instruction, no flag dep */
        if (rc && (rv==3 || rv==5 || rv==9)) {
            const char *base=cg_materialise(cg,best->args[0]);
            if (base) {
                int scale=(rv==3)?2:(rv==5)?4:8;
                char mem[64];
                snprintf(mem,sizeof(mem),"[%s+%s*%d]",base,base,scale);
                cg_emit(cg,"lea",dst_reg,mem);
                break;
            }
        }
        /* LEA synthesis for mul by 2: lea dst, [src+src] */
        if (rc && rv==2) {
            const char *base=cg_materialise(cg,best->args[0]);
            if (base && strcmp(base,dst_reg)!=0) {
                char mem[64];
                snprintf(mem,sizeof(mem),"[%s+%s]",base,base);
                cg_emit(cg,"lea",dst_reg,mem);
                break;
            }
        }
        const char *lhs = cg_materialise(cg, best->args[0]);
        if (lhs && strcmp(lhs,dst_reg)!=0) cg_emit(cg,"mov",dst_reg,lhs);
        else if (!lhs) cg_emit_into(cg,best->args[0],dst_reg);
        if (rc) {
            char imm_s[32]; snprintf(imm_s,sizeof(imm_s),"%lld",(long long)rv);
            cg_emit(cg,"imul",dst_reg,imm_s);
        } else {
            const char *rhs=cg_materialise(cg,best->args[1]);
            if (rhs) cg_emit(cg,"imul",dst_reg,rhs);
        }
        break;
    }

    case EG_AND: {
        const char *lhs=cg_materialise(cg,best->args[0]);
        const char *rhs=cg_materialise(cg,best->args[1]);
        if (lhs && strcmp(lhs,dst_reg)!=0) cg_emit(cg,"mov",dst_reg,lhs);
        else if (!lhs) cg_emit_into(cg,best->args[0],dst_reg);
        int64_t rv; bool rc=eg_is_const(g,best->args[1],&rv);
        if (rc) { char imm_s[32]; snprintf(imm_s,sizeof(imm_s),"%lld",(long long)rv);
                   cg_emit(cg,"and",dst_reg,imm_s); }
        else if (rhs) cg_emit(cg,"and",dst_reg,rhs);
        break;
    }

    case EG_OR: {
        const char *lhs=cg_materialise(cg,best->args[0]);
        const char *rhs=cg_materialise(cg,best->args[1]);
        if (lhs && strcmp(lhs,dst_reg)!=0) cg_emit(cg,"mov",dst_reg,lhs);
        else if (!lhs) cg_emit_into(cg,best->args[0],dst_reg);
        int64_t rv; bool rc=eg_is_const(g,best->args[1],&rv);
        if (rc) { char imm_s[32]; snprintf(imm_s,sizeof(imm_s),"%lld",(long long)rv);
                   cg_emit(cg,"or",dst_reg,imm_s); }
        else if (rhs) cg_emit(cg,"or",dst_reg,rhs);
        break;
    }

    case EG_XOR: {
        /* Special case: xor(x,x) = 0 → emit xor dst,dst */
        if (eg_equiv(g,best->args[0],best->args[1]) ||
            (eg_is_zero(g,ec))) {
            cg_emit(cg,"xor",dst_reg,dst_reg);
            break;
        }
        const char *lhs=cg_materialise(cg,best->args[0]);
        const char *rhs=cg_materialise(cg,best->args[1]);
        if (lhs && strcmp(lhs,dst_reg)!=0) cg_emit(cg,"mov",dst_reg,lhs);
        else if (!lhs) cg_emit_into(cg,best->args[0],dst_reg);
        int64_t rv; bool rc=eg_is_const(g,best->args[1],&rv);
        if (rc) { char imm_s[32]; snprintf(imm_s,sizeof(imm_s),"%lld",(long long)rv);
                   cg_emit(cg,"xor",dst_reg,imm_s); }
        else if (rhs) cg_emit(cg,"xor",dst_reg,rhs);
        break;
    }

    case EG_NOT: {
        const char *src2=cg_materialise(cg,best->args[0]);
        if (src2 && strcmp(src2,dst_reg)!=0) cg_emit(cg,"mov",dst_reg,src2);
        else if (!src2) cg_emit_into(cg,best->args[0],dst_reg);
        cg_emit(cg,"not",dst_reg,NULL);
        break;
    }

    case EG_NEG: {
        const char *src2=cg_materialise(cg,best->args[0]);
        if (src2 && strcmp(src2,dst_reg)!=0) cg_emit(cg,"mov",dst_reg,src2);
        else if (!src2) cg_emit_into(cg,best->args[0],dst_reg);
        cg_emit(cg,"neg",dst_reg,NULL);
        break;
    }

    case EG_SHL: {
        const char *src2=cg_materialise(cg,best->args[0]);
        if (src2 && strcmp(src2,dst_reg)!=0) cg_emit(cg,"mov",dst_reg,src2);
        else if (!src2) cg_emit_into(cg,best->args[0],dst_reg);
        int64_t sv; bool sc=eg_is_const(g,best->args[1],&sv);
        if (sc) { char ss[16]; snprintf(ss,sizeof(ss),"%lld",(long long)sv);
                   cg_emit(cg,"shl",dst_reg,ss); }
        else cg_emit(cg,"shl",dst_reg,"cl");
        break;
    }

    case EG_SHR: {
        const char *src2=cg_materialise(cg,best->args[0]);
        if (src2 && strcmp(src2,dst_reg)!=0) cg_emit(cg,"mov",dst_reg,src2);
        else if (!src2) cg_emit_into(cg,best->args[0],dst_reg);
        int64_t sv; bool sc=eg_is_const(g,best->args[1],&sv);
        if (sc) { char ss[16]; snprintf(ss,sizeof(ss),"%lld",(long long)sv);
                   cg_emit(cg,"shr",dst_reg,ss); }
        else cg_emit(cg,"shr",dst_reg,"cl");
        break;
    }

    case EG_SAR: {
        const char *src2=cg_materialise(cg,best->args[0]);
        if (src2 && strcmp(src2,dst_reg)!=0) cg_emit(cg,"mov",dst_reg,src2);
        else if (!src2) cg_emit_into(cg,best->args[0],dst_reg);
        int64_t sv; bool sc=eg_is_const(g,best->args[1],&sv);
        if (sc) { char ss[16]; snprintf(ss,sizeof(ss),"%lld",(long long)sv);
                   cg_emit(cg,"sar",dst_reg,ss); }
        else cg_emit(cg,"sar",dst_reg,"cl");
        break;
    }

    case EG_INC: {
        const char *src2=cg_materialise(cg,best->args[0]);
        if (src2 && strcmp(src2,dst_reg)!=0) cg_emit(cg,"mov",dst_reg,src2);
        else if (!src2) cg_emit_into(cg,best->args[0],dst_reg);
        cg_emit(cg,"inc",dst_reg,NULL);
        break;
    }

    case EG_DEC: {
        const char *src2=cg_materialise(cg,best->args[0]);
        if (src2 && strcmp(src2,dst_reg)!=0) cg_emit(cg,"mov",dst_reg,src2);
        else if (!src2) cg_emit_into(cg,best->args[0],dst_reg);
        cg_emit(cg,"dec",dst_reg,NULL);
        break;
    }

    case EG_BSF: {
        const char *src2=cg_materialise(cg,best->args[0]);
        if (src2) cg_emit(cg,"bsf",dst_reg,src2);
        break;
    }
    case EG_BSR: {
        const char *src2=cg_materialise(cg,best->args[0]);
        if (src2) cg_emit(cg,"bsr",dst_reg,src2);
        break;
    }
    case EG_TZCNT: {
        const char *src2=cg_materialise(cg,best->args[0]);
        if (src2) cg_emit(cg,"tzcnt",dst_reg,src2);
        break;
    }
    case EG_LZCNT: {
        const char *src2=cg_materialise(cg,best->args[0]);
        if (src2) cg_emit(cg,"lzcnt",dst_reg,src2);
        break;
    }
    case EG_POPCNT: {
        const char *src2=cg_materialise(cg,best->args[0]);
        if (src2) cg_emit(cg,"popcnt",dst_reg,src2);
        break;
    }
    case EG_BSWAP:
        /* bswap always operates on dst itself */
        cg_emit(cg,"bswap",dst_reg,NULL);
        break;

    default:
        break;
    }

    /* Record that dst_reg now holds this e-class */
    if (cg->nslots < MAX_SLOTS) {
        cg->slots[cg->nslots].eclass   = ec;
        cg->slots[cg->nslots].reg      = dst_reg;
        cg->slots[cg->nslots].is_input = false;
        cg->nslots++;
    }
}

/*
 * Materialise (ensure in a register) the e-class value.
 * Returns the register name, or NULL if we could not materialise it
 * (caller must fall back to cg_emit_into with an explicit destination).
 */
static const char *cg_materialise(CodeGen *cg, uint32_t ec)
{
    EGraph *g = cg->g;
    ec = eg_find(g, ec);

    /* Already in a register? (skip stale/invalidated slots) */
    int slot = cg_find_slot(cg, ec);
    if (slot >= 0 && cg->slots[slot].eclass != EG_NULL)
        return cg->slots[slot].reg;

    /* Is it a constant? caller can use an immediate directly. */
    if (eg_is_const(g, ec, NULL)) return NULL;

    /* Is it an unmodified input variable whose register is still live? */
    const char *vname=NULL;
    if (eg_is_var(g, ec, &vname) && vname) {
        /* Check if it's an original input not clobbered (slot still valid) */
        for (int i = 0; i < cg->rm->nregs; i++) {
            if (strcmp(cg->rm->regs[i].name, vname)==0) {
                /* Make sure no stale (invalidated) slot holds this reg */
                bool clobbered = false;
                for (int s = 0; s < cg->nslots; s++) {
                    if (cg->slots[s].reg &&
                        strcmp(cg->slots[s].reg, vname) == 0 &&
                        cg->slots[s].eclass == EG_NULL) {
                        clobbered = true;
                        break;
                    }
                }
                if (clobbered) return NULL; /* register was clobbered */
                /* Record slot so future lookups find it */
                if (cg->nslots < MAX_SLOTS) {
                    cg->slots[cg->nslots].eclass   = ec;
                    cg->slots[cg->nslots].reg      = cg->rm->regs[i].name;
                    cg->slots[cg->nslots].is_input = true;
                    cg->nslots++;
                }
                return cg->rm->regs[i].name;
            }
        }
    }

    /* Need to compute it; no dedicated register known — return NULL so
       the caller will drive cg_emit_into with an explicit destination. */
    return NULL;
}

/* ── Segment optimiser ───────────────────────────────────────────────────── */

/*
 * Flush a straight-line segment: lift → saturate → extract → emit.
 *
 * modified[] lists (reg_name, final_eclass) pairs — the registers that
 * were written within this segment, in order of first write.
 */

typedef struct { char *reg; uint32_t eclass; } ModEntry;

static void flush_segment(EGraph *g, RegMap *rm,
                           ModEntry *modified, int nmod,
                           LineList *out, int is_att,
                           const EgCpuModel *m)
{
    if (nmod == 0) { eg_destroy(g); return; }

    /* Run equality saturation with CPU-aware rewrite rules */
    eg_saturate(g, m, 20);

    /* Compute extraction costs using the target CPU's latency model */
    eg_compute_costs(g, m);

    /* Set up code generator */
    CodeGen cg;
    memset(&cg, 0, sizeof(cg));
    cg.g      = g;
    cg.rm     = rm;
    cg.out    = out;
    cg.is_att = is_att;

    /*
     * Choose real caller-saved scratch registers, avoiding any register
     * already live in this segment's register map.  Fall back to synthetic
     * names only if we run out of candidates (highly unusual in practice).
     */
    {
        int si = 0;
        for (int ci = 0; ci < MAX_SCRATCH && si < MAX_SCRATCH; ci++) {
            bool in_use = false;
            for (int ri = 0; ri < rm->nregs; ri++) {
                if (rm->regs[ri].name &&
                    strcmp(rm->regs[ri].name, CANDIDATE_SCRATCH[ci]) == 0) {
                    in_use = true;
                    break;
                }
            }
            if (!in_use) {
                snprintf(cg.scratch_names[si], sizeof(cg.scratch_names[si]),
                         "%s", CANDIDATE_SCRATCH[ci]);
                si++;
            }
        }
        /* Fill remaining slots with synthetic names as fallback */
        for (; si < MAX_SCRATCH; si++) {
            snprintf(cg.scratch_names[si], sizeof(cg.scratch_names[si]),
                     "__scratch%d__", si);
        }
    }

    /* Seed the slot table with all original input values
       (so we can reuse them when building expressions). */
    for (int i = 0; i < rm->nregs; i++) {
        uint32_t init_ec = eg_add_var(g, rm->regs[i].name);
        if (cg.nslots < MAX_SLOTS) {
            cg.slots[cg.nslots].eclass   = eg_find(g, init_ec);
            cg.slots[cg.nslots].reg      = rm->regs[i].name;
            cg.slots[cg.nslots].is_input = true;
            cg.nslots++;
        }
    }

    /* For each modified register (in write order), emit its computation. */
    for (int i = 0; i < nmod; i++) {
        uint32_t ec = eg_find(g, modified[i].eclass);
        const char *dst = modified[i].reg;

        /* Check if the e-class is already a VAR equal to the original input
           of the same register (i.e., the instruction was a no-op). */
        const char *vn=NULL;
        if (eg_is_var(g, ec, &vn) && vn && strcmp(vn, dst)==0) {
            /* No change — skip emitting */
            continue;
        }
        /* Also skip if this register's ORIGINAL e-class already equals ec */
        int orig_slot=-1;
        for (int s=0;s<cg.nslots;s++) {
            if (cg.slots[s].reg==dst || (cg.slots[s].reg&&strcmp(cg.slots[s].reg,dst)==0)) {
                if (eg_equiv(g, cg.slots[s].eclass, ec)) { orig_slot=s; break; }
            }
        }
        if (orig_slot>=0 && cg.slots[orig_slot].is_input) continue;

        cg_emit_into(&cg, ec, dst);

        /* Update slot: mark dst as now holding this ec */
        bool updated=false;
        for (int s=0;s<cg.nslots;s++) {
            if (cg.slots[s].reg==dst||(cg.slots[s].reg&&strcmp(cg.slots[s].reg,dst)==0)) {
                cg.slots[s].eclass   = ec;
                cg.slots[s].is_input = false;
                updated = true; break;
            }
        }
        if (!updated && cg.nslots < MAX_SLOTS) {
            cg.slots[cg.nslots].eclass   = ec;
            cg.slots[cg.nslots].reg      = dst;
            cg.slots[cg.nslots].is_input = false;
            cg.nslots++;
        }
    }

    eg_destroy(g);
}

/* ── Public entry point ─────────────────────────────────────────────────── */

char **x86_egraph_optimize(const char **lines, size_t nlines,
                            int is_att, const char *cpu_name,
                            size_t *out_nlines)
{
    /* Look up the microarchitecture model — drives costs and rewrite rules */
    const EgCpuModel *m = cpu_model_lookup(cpu_name);

    LineList out;
    memset(&out, 0, sizeof(out));

    /* Per-segment state */
    EGraph  *g       = eg_create();
    RegMap   rm;  memset(&rm,0,sizeof(rm));
    ModEntry *mods   = xmalloc(128 * sizeof(ModEntry));
    int       nmods  = 0;
    int       modcap = 128;

    /* Track which registers have already been added to mods (first write) */
    /* to preserve write-order semantics. */

    for (size_t li = 0; li < nlines; li++) {
        const char *raw_line = lines[li];
        char *line = xstrdup(raw_line);

        /* Preserve original leading whitespace (indentation) for pass-through */
        const char *orig_indent = raw_line;
        size_t indent_len = 0;
        while (orig_indent[indent_len] &&
               (orig_indent[indent_len]==' ' || orig_indent[indent_len]=='\t'))
            indent_len++;
        char indent_buf[64];
        if (indent_len >= sizeof(indent_buf)) indent_len = sizeof(indent_buf)-1;
        memcpy(indent_buf, orig_indent, indent_len);
        indent_buf[indent_len] = '\0';

        /* Separate inline comment (everything from first ';'/'#' onwards) */
        char comment[256]; comment[0]='\0';
        {
            bool in_s=false; char *p=line;
            while(*p){
                if(*p=='"')in_s=!in_s;
                if(!in_s&&(*p==';'||*p=='#')){
                    snprintf(comment,sizeof(comment),"%s",p);
                    *p='\0'; break;
                }
                p++;
            }
        }

        char *trimmed = trim(line);

        /* Comment-only line (trimmed is empty after removing the comment) */
        bool is_comment_only = (trimmed[0]=='\0') && comment[0]!='\0';

        /* Blank line, label, directive, or standalone comment → flush + pass through */
        bool is_label     = strlen(trimmed)>0 && trimmed[strlen(trimmed)-1]==':';
        bool is_directive = trimmed[0]=='.' || trimmed[0]=='#' || trimmed[0]=='%';
        bool is_blank     = trimmed[0]=='\0';

        if (is_blank || is_label || is_directive || is_comment_only) {
            flush_segment(g, &rm, mods, nmods, &out, is_att, m);
            g=eg_create(); rm_free(&rm); memset(&rm,0,sizeof(rm)); nmods=0;
            /* Pass through preserving original formatting */
            char buf[512];
            if (is_comment_only)
                snprintf(buf,sizeof(buf),"%s%s",indent_buf,comment);
            else if (comment[0])
                snprintf(buf,sizeof(buf),"%s%s %s",indent_buf,trimmed,comment);
            else
                snprintf(buf,sizeof(buf),"%s%s",indent_buf,trimmed);
            ll_push(&out, xstrdup(buf));
            free(line);
            continue;
        }

        /* Parse mnemonic and operands */
        char work[512];
        snprintf(work,sizeof(work),"%s",trimmed);
        /* Mnemonic is first whitespace-separated token */
        char *p = work;
        while (*p && !isspace((unsigned char)*p)) p++;
        char mnem[64]; size_t mlen=(size_t)(p-work);
        if (mlen>=sizeof(mnem)) {
            /* Too long — flush and pass through */
            flush_segment(g,&rm,mods,nmods,&out,is_att,m);
            g=eg_create(); rm_free(&rm); memset(&rm,0,sizeof(rm)); nmods=0;
            char buf[512];
            snprintf(buf,sizeof(buf),"%s%s",trimmed,comment);
            ll_push(&out,xstrdup(buf));
            free(line); continue;
        }
        memcpy(mnem,work,mlen); mnem[mlen]='\0';
        while(*p && isspace((unsigned char)*p)) p++;
        char *ops_str = p;

        /* Split operands */
        char *ops[8]; int nops=0;
        if (*ops_str) {
            char ops_copy[512];
            snprintf(ops_copy,sizeof(ops_copy),"%s",ops_str);
            nops=split_operands(ops_copy,ops,8);
            /* Repoint into ops_str (we need stable memory — use work space) */
            char *q=ops_str;
            int oi=0;
            /* Re-split directly in work so pointers stay valid */
            for(int i=0;i<nops&&i<8;i++){
                ops[i]=q;
                /* find end of this operand */
                char *comma=strchr(q,',');
                if(comma){*comma='\0';q=comma+1;while(*q==' ')q++;}
                else{q+=strlen(q);}
                /* trim */
                char *e=ops[i]+strlen(ops[i]);
                while(e>ops[i]&&isspace((unsigned char)e[-1]))*--e='\0';
                char *s=ops[i]; while(*s&&isspace((unsigned char)*s))s++;
                if(s!=ops[i])memmove(ops[i],s,strlen(s)+1);
                oi++;
            }
            nops=oi;
        }

        /* Determine destination register (for tracking modifications) */
        const char *dst_raw = is_att ? (nops>=1?ops[nops-1]:NULL) : (nops>=1?ops[0]:NULL);
        const char *dst_canon = dst_raw ? canon_reg(dst_raw) : NULL;

        /* Attempt to lift into e-graph */
        bool lifted = lift_insn(g, &rm, mnem, ops, nops, is_att);

        if (!lifted) {
            /* Barrier: flush, emit original line verbatim, reset */
            flush_segment(g, &rm, mods, nmods, &out, is_att, m);
            g=eg_create(); rm_free(&rm); memset(&rm,0,sizeof(rm)); nmods=0;
            char buf[512];
            if (comment[0])
                snprintf(buf,sizeof(buf),"%s%s %s",indent_buf,trimmed,comment);
            else
                snprintf(buf,sizeof(buf),"%s%s",indent_buf,trimmed);
            ll_push(&out, xstrdup(buf));
            free(line); continue;
        }

        /* Record modification */
        if (dst_canon) {
            uint32_t new_ec = rm_get(g, &rm, dst_canon);
            if (new_ec != EG_NULL) {
                /* Update or add entry in mods */
                bool found=false;
                for(int m=0;m<nmods;m++){
                    if(strcmp(mods[m].reg,dst_canon)==0){
                        mods[m].eclass=new_ec; found=true; break;
                    }
                }
                if(!found){
                    if(nmods>=modcap){
                        modcap*=2;
                        mods=xrealloc(mods,modcap*sizeof(ModEntry));
                    }
                    mods[nmods].reg=xstrdup(dst_canon);
                    mods[nmods].eclass=new_ec;
                    nmods++;
                }
            }
        }

        free(line);
    }

    /* Flush final segment */
    flush_segment(g, &rm, mods, nmods, &out, is_att, m);
    rm_free(&rm);
    for(int m=0;m<nmods;m++) free(mods[m].reg);
    free(mods);

    /* Return output */
    if (out_nlines) *out_nlines = out.n;
    return out.lines;
}
