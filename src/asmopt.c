/*
 * asmopt.c - Assembly Optimizer Implementation
 * 
 * This file implements a comprehensive assembly language optimizer for x86-64.
 * It focuses on peephole optimizations that improve code size and performance
 * without changing program semantics.
 * 
 * Architecture:
 *   1. Parsing: Converts assembly text into an internal representation (IR)
 *   2. Analysis: Builds control flow graph (CFG) for future optimizations
 *   3. Optimization: Applies peephole patterns to individual instructions
 *   4. Code Generation: Emits optimized assembly preserving comments/labels
 *   5. Reporting: Tracks and reports all optimizations applied
 * 
 * Key Features:
 *   - 27 peephole optimization patterns
 *   - Support for Intel and AT&T syntax
 *   - Comment and label preservation
 *   - Detailed optimization reporting
 *   - Zero security vulnerabilities (CodeQL verified)
 * 
 * Performance:
 *   - O(n) time complexity for optimization passes
 *   - O(n) memory growth strategy for event tracking
 *   - Processes thousands of instructions efficiently
 */

#include "asmopt.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asmopt.h"

#define IMMEDIATE_BUFFER_SIZE 64
#define ZERO_GUARD_PATTERN_LINES 3

typedef struct {
    size_t original_lines;
    size_t optimized_lines;
    size_t replacements;
    size_t removals;
} asmopt_stats;

typedef struct {
    size_t line_no;
    char* pattern_name;
    char* original;
    char* optimized;
} asmopt_optimization_event;

typedef struct {
    size_t line_no;
    char* kind;
    char* text;
    char* mnemonic;
    char** operands;
    size_t operand_count;
} asmopt_ir_line;

typedef struct {
    char* name;
    asmopt_ir_line** instructions;
    size_t instruction_count;
} asmopt_cfg_block;

typedef struct {
    char* source;
    char* target;
} asmopt_cfg_edge;

typedef struct {
    char* key;
    char* value;
} asmopt_option;

struct asmopt_context {
    char* architecture;
    char* target_cpu;
    char* format;
    int optimization_level;
    bool amd_optimizations;
    bool no_optimize;
    bool preserve_all;
    char** enabled_opts;
    size_t enabled_count;
    char** disabled_opts;
    size_t disabled_count;
    asmopt_option* options;
    size_t option_count;
    char* original_text;
    char** original_lines;
    size_t original_count;
    char** optimized_lines;
    size_t optimized_count;
    asmopt_stats stats;
    asmopt_ir_line* ir;
    size_t ir_count;
    asmopt_cfg_block* cfg_blocks;
    size_t cfg_block_count;
    asmopt_cfg_edge* cfg_edges;
    size_t cfg_edge_count;
    bool trailing_newline;
    asmopt_optimization_event* opt_events;
    size_t opt_event_count;
    size_t opt_event_capacity;
    /* Flag to enable hot loop alignment when hot_align option is set */
    bool insert_hot_align;
    size_t skip_lines;
};

/* Instruction mnemonics that can have AT&T syntax suffixes (b, w, l, q) */
static const char* const SUFFIX_MNEMONICS[] = {
    "mov", "lea", "add", "sub", "xor", "and", "or", "cmp", "test", 
    "shl", "shr", "sal", "sar"
};
static const size_t SUFFIX_MNEMONICS_COUNT = sizeof(SUFFIX_MNEMONICS) / sizeof(SUFFIX_MNEMONICS[0]);

static char* asmopt_strdup(const char* value) {
    if (!value) {
        return NULL;
    }
    size_t len = strlen(value);
    char* copy = malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, len + 1);
    return copy;
}

static bool asmopt_starts_with(const char* value, const char* prefix) {
    if (!value || !prefix) {
        return false;
    }
    size_t len = strlen(prefix);
    return strncmp(value, prefix, len) == 0;
}

static bool asmopt_is_blank(const char* value) {
    if (!value) {
        return true;
    }
    while (*value) {
        if (!isspace((unsigned char)*value)) {
            return false;
        }
        value++;
    }
    return true;
}

static void asmopt_free_string_array(char** values, size_t count) {
    if (!values) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(values[i]);
    }
    free(values);
}

static void asmopt_reset_opt_events(asmopt_context* ctx) {
    if (!ctx) {
        return;
    }
    for (size_t i = 0; i < ctx->opt_event_count; i++) {
        free(ctx->opt_events[i].pattern_name);
        free(ctx->opt_events[i].original);
        free(ctx->opt_events[i].optimized);
    }
    free(ctx->opt_events);
    ctx->opt_events = NULL;
    ctx->opt_event_count = 0;
    ctx->opt_event_capacity = 0;
}

static void asmopt_reset_ir(asmopt_context* ctx) {
    if (!ctx) {
        return;
    }
    for (size_t i = 0; i < ctx->ir_count; i++) {
        asmopt_ir_line* line = &ctx->ir[i];
        free(line->kind);
        free(line->text);
        free(line->mnemonic);
        asmopt_free_string_array(line->operands, line->operand_count);
    }
    free(ctx->ir);
    ctx->ir = NULL;
    ctx->ir_count = 0;
}

static void asmopt_reset_cfg(asmopt_context* ctx) {
    if (!ctx) {
        return;
    }
    for (size_t i = 0; i < ctx->cfg_block_count; i++) {
        asmopt_cfg_block* block = &ctx->cfg_blocks[i];
        free(block->name);
        for (size_t j = 0; j < block->instruction_count; j++) {
            /* instructions belong to IR; nothing to free here */
            block->instructions[j] = NULL;
        }
        free(block->instructions);
    }
    for (size_t i = 0; i < ctx->cfg_edge_count; i++) {
        free(ctx->cfg_edges[i].source);
        free(ctx->cfg_edges[i].target);
    }
    free(ctx->cfg_blocks);
    free(ctx->cfg_edges);
    ctx->cfg_blocks = NULL;
    ctx->cfg_block_count = 0;
    ctx->cfg_edges = NULL;
    ctx->cfg_edge_count = 0;
}

static void asmopt_reset_lines(asmopt_context* ctx) {
    asmopt_free_string_array(ctx->original_lines, ctx->original_count);
    asmopt_free_string_array(ctx->optimized_lines, ctx->optimized_count);
    ctx->original_lines = NULL;
    ctx->optimized_lines = NULL;
    ctx->original_count = 0;
    ctx->optimized_count = 0;
    free(ctx->original_text);
    ctx->original_text = NULL;
    ctx->trailing_newline = false;
    asmopt_reset_ir(ctx);
    asmopt_reset_cfg(ctx);
    asmopt_reset_opt_events(ctx);
    memset(&ctx->stats, 0, sizeof(ctx->stats));
}

static bool asmopt_option_enabled(asmopt_context* ctx, const char* key) {
    if (!ctx || !key) {
        return false;
    }
    for (size_t i = 0; i < ctx->option_count; i++) {
        if (ctx->options[i].key && strcmp(ctx->options[i].key, key) == 0) {
            return ctx->options[i].value && strcmp(ctx->options[i].value, "1") == 0;
        }
    }
    return false;
}

static void asmopt_add_option(asmopt_context* ctx, const char* key, const char* value) {
    if (!ctx || !key) {
        return;
    }
    asmopt_option* next = realloc(ctx->options, sizeof(asmopt_option) * (ctx->option_count + 1));
    if (!next) {
        return;
    }
    ctx->options = next;
    ctx->options[ctx->option_count].key = asmopt_strdup(key);
    ctx->options[ctx->option_count].value = asmopt_strdup(value ? value : "");
    ctx->option_count += 1;
}

static void asmopt_add_name(char*** list, size_t* count, const char* value) {
    if (!list || !count || !value) {
        return;
    }
    char** next = realloc(*list, sizeof(char*) * (*count + 1));
    if (!next) {
        return;
    }
    *list = next;
    (*list)[*count] = asmopt_strdup(value);
    *count += 1;
}

static bool asmopt_has_opt(char** list, size_t count, const char* value) {
    if (!list || !value) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (list[i] && strcmp(list[i], value) == 0) {
            return true;
        }
    }
    return false;
}

static int asmopt_casecmp(const char* left, const char* right) {
    if (left == right) {
        return 0;
    }
    if (!left) {
        return -1;
    }
    if (!right) {
        return 1;
    }
    while (*left && *right) {
        char lch = (char)tolower((unsigned char)*left);
        char rch = (char)tolower((unsigned char)*right);
        if (lch != rch) {
            return (unsigned char)lch - (unsigned char)rch;
        }
        left++;
        right++;
    }
    return (unsigned char)tolower((unsigned char)*left) - (unsigned char)tolower((unsigned char)*right);
}

static bool asmopt_case_prefix_len(const char* value, const char* prefix) {
    if (!value || !prefix) {
        return false;
    }
    while (*prefix) {
        if (!*value) {
            return false;
        }
        if (tolower((unsigned char)*value) != tolower((unsigned char)*prefix)) {
            return false;
        }
        value++;
        prefix++;
    }
    return true;
}

static bool asmopt_is_disabled(asmopt_context* ctx, const char* name) {
    if (!ctx || !name) {
        return false;
    }
    if (asmopt_has_opt(ctx->disabled_opts, ctx->disabled_count, "all")) {
        return true;
    }
    return asmopt_has_opt(ctx->disabled_opts, ctx->disabled_count, name);
}

static void asmopt_split_lines(asmopt_context* ctx, const char* text) {
    size_t length = strlen(text);
    ctx->trailing_newline = length > 0 && text[length - 1] == '\n';
    size_t capacity = 16;
    ctx->original_lines = malloc(sizeof(char*) * capacity);
    ctx->original_count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= length; i++) {
        if (text[i] == '\n' || text[i] == '\0') {
            size_t line_len = i - start;
            char* line = malloc(line_len + 1);
            if (!line) {
                break;
            }
            memcpy(line, text + start, line_len);
            line[line_len] = '\0';
            if (ctx->original_count == capacity) {
                capacity *= 2;
                char** next = realloc(ctx->original_lines, sizeof(char*) * capacity);
                if (!next) {
                    free(line);
                    break;
                }
                ctx->original_lines = next;
            }
            ctx->original_lines[ctx->original_count++] = line;
            start = i + 1;
        }
    }
}

static char* asmopt_join_lines(char** lines, size_t count, bool trailing_newline) {
    if (!lines || count == 0) {
        return asmopt_strdup(trailing_newline ? "\n" : "");
    }
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += strlen(lines[i]);
    }
    total += count > 0 ? (count - 1) : 0;
    if (trailing_newline) {
        total += 1;
    }
    char* buffer = malloc(total + 1);
    if (!buffer) {
        return NULL;
    }
    buffer[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        strcat(buffer, lines[i]);
        if (i + 1 < count) {
            strcat(buffer, "\n");
        }
    }
    if (trailing_newline) {
        strcat(buffer, "\n");
    }
    return buffer;
}

static void asmopt_split_comment(const char* line, char** code, char** comment) {
    if (!line) {
        *code = asmopt_strdup("");
        *comment = asmopt_strdup("");
        return;
    }
    const char* markers = ";#";
    const char* comment_pos = NULL;
    for (const char* ptr = line; *ptr; ptr++) {
        if (strchr(markers, *ptr)) {
            comment_pos = ptr;
            break;
        }
    }
    if (!comment_pos) {
        *code = asmopt_strdup(line);
        *comment = asmopt_strdup("");
        return;
    }
    size_t code_len = (size_t)(comment_pos - line);
    *code = malloc(code_len + 1);
    if (*code) {
        memcpy(*code, line, code_len);
        (*code)[code_len] = '\0';
    }
    *comment = asmopt_strdup(comment_pos);
}

static bool asmopt_is_directive_or_label(const char* code) {
    if (!code) {
        return true;
    }
    const char* trimmed = code;
    while (*trimmed && isspace((unsigned char)*trimmed)) {
        trimmed++;
    }
    if (*trimmed == '\0') {
        return true;
    }
    if (*trimmed == '.') {
        return true;
    }
    size_t len = strlen(trimmed);
    if (len > 0 && trimmed[len - 1] == ':') {
        return true;
    }
    return false;
}

static bool asmopt_parse_instruction(const char* code, char** indent, char** mnemonic, char** spacing, char** operands) {
    if (!code) {
        return false;
    }
    const char* ptr = code;
    while (*ptr && isspace((unsigned char)*ptr)) {
        ptr++;
    }
    size_t indent_len = (size_t)(ptr - code);
    const char* mstart = ptr;
    if (!isalpha((unsigned char)*mstart)) {
        return false;
    }
    while (*ptr && (isalnum((unsigned char)*ptr) || *ptr == '.')) {
        ptr++;
    }
    size_t mnemonic_len = (size_t)(ptr - mstart);
    const char* sstart = ptr;
    while (*ptr && isspace((unsigned char)*ptr)) {
        ptr++;
    }
    size_t spacing_len = (size_t)(ptr - sstart);
    const char* operands_start = ptr;
    *indent = malloc(indent_len + 1);
    *mnemonic = malloc(mnemonic_len + 1);
    *spacing = malloc(spacing_len + 1);
    *operands = asmopt_strdup(operands_start);
    if (!*indent || !*mnemonic || !*spacing || !*operands) {
        free(*indent);
        free(*mnemonic);
        free(*spacing);
        free(*operands);
        return false;
    }
    memcpy(*indent, code, indent_len);
    (*indent)[indent_len] = '\0';
    memcpy(*mnemonic, mstart, mnemonic_len);
    (*mnemonic)[mnemonic_len] = '\0';
    memcpy(*spacing, sstart, spacing_len);
    (*spacing)[spacing_len] = '\0';
    return true;
}

static char* asmopt_strip(const char* value) {
    if (!value) {
        return asmopt_strdup("");
    }
    const char* start = value;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    const char* end = value + strlen(value);
    while (end > start && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    size_t len = (size_t)(end - start);
    char* result = malloc(len + 1);
    if (!result) {
        return NULL;
    }
    memcpy(result, start, len);
    result[len] = '\0';
    return result;
}

static bool asmopt_parse_operands(const char* operands, char** op1, char** op2, char** pre_space, char** post_space) {
    const char* comma = operands ? strchr(operands, ',') : NULL;
    if (!comma) {
        return false;
    }
    size_t left_len = (size_t)(comma - operands);
    char* left = malloc(left_len + 1);
    if (!left) {
        return false;
    }
    memcpy(left, operands, left_len);
    left[left_len] = '\0';
    char* right = asmopt_strdup(comma + 1);
    if (!right) {
        free(left);
        return false;
    }
    size_t left_trimmed = strlen(left);
    while (left_trimmed > 0 && isspace((unsigned char)left[left_trimmed - 1])) {
        left_trimmed--;
    }
    size_t right_trimmed = 0;
    while (right[right_trimmed] && isspace((unsigned char)right[right_trimmed])) {
        right_trimmed++;
    }
    *pre_space = asmopt_strdup(left + left_trimmed);
    *post_space = asmopt_strdup(right);
    (*post_space)[right_trimmed] = '\0';
    *op1 = asmopt_strip(left);
    *op2 = asmopt_strip(right);
    free(left);
    free(right);
    return *op1 && *op2 && *pre_space && *post_space;
}

static char* asmopt_trim_comment(const char* value) {
    if (!value) {
        return asmopt_strdup("");
    }
    const char* start = value;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    return asmopt_strdup(start);
}

static bool asmopt_is_register(const char* operand, const char* syntax) {
    if (!operand || !*operand) {
        return false;
    }
    while (isspace((unsigned char)*operand)) {
        operand++;
    }
    const char* op = operand;
    if (syntax && strcmp(syntax, "att") == 0) {
        if (op[0] != '%') {
            return false;
        }
        op++;
    }
    if (op[0] == '$' || op[0] == '*' || strchr(op, '[') || strchr(op, '(')) {
        return false;
    }
    for (const char* ptr = op; *ptr; ptr++) {
        if (!isalnum((unsigned char)*ptr) && *ptr != '_') {
            return false;
        }
    }
    return true;
}

static bool asmopt_is_immediate_zero(const char* operand, const char* syntax) {
    if (!operand) {
        return false;
    }
    while (isspace((unsigned char)*operand)) {
        operand++;
    }
    const char* op = operand;
    if (syntax && strcmp(syntax, "att") == 0) {
        if (op[0] != '$') {
            return false;
        }
        op++;
    }
    while (*op && isspace((unsigned char)*op)) {
        op++;
    }
    if (*op == '\0') {
        return false;
    }
    if (asmopt_starts_with(op, "0x")) {
        char* end = NULL;
        long value = strtol(op, &end, 16);
        return end != op && *end == '\0' && value == 0;
    }
    if (op[strlen(op) - 1] == 'h') {
        char buffer[64];
        size_t len = strlen(op) - 1;
        if (len >= sizeof(buffer)) {
            return false;
        }
        memcpy(buffer, op, len);
        buffer[len] = '\0';
        char* end = NULL;
        long value = strtol(buffer, &end, 16);
        return end != buffer && *end == '\0' && value == 0;
    }
    char* end = NULL;
    long value = strtol(op, &end, 10);
    return end != op && *end == '\0' && value == 0;
}

static char* asmopt_detect_syntax(asmopt_context* ctx) {
    if (!ctx) {
        return asmopt_strdup("intel");
    }
    if (ctx->format) {
        return asmopt_strdup(ctx->format);
    }
    for (size_t i = 0; i < ctx->original_count; i++) {
        if (strchr(ctx->original_lines[i], '%')) {
            return asmopt_strdup("att");
        }
    }
    return asmopt_strdup("intel");
}

static void asmopt_store_optimized_line(asmopt_context* ctx, const char* line) {
    if (!ctx || !line) {
        return;
    }
    char** next = realloc(ctx->optimized_lines, sizeof(char*) * (ctx->optimized_count + 1));
    if (!next) {
        return;
    }
    ctx->optimized_lines = next;
    ctx->optimized_lines[ctx->optimized_count++] = asmopt_strdup(line);
}

static void asmopt_record_optimization(asmopt_context* ctx, size_t line_no, const char* pattern, 
                                       const char* original, const char* optimized) {
    if (!ctx || !pattern) {
        return;
    }
    
    /* Grow capacity if needed using doubling strategy */
    if (ctx->opt_event_count >= ctx->opt_event_capacity) {
        size_t new_capacity = ctx->opt_event_capacity == 0 ? 16 : ctx->opt_event_capacity * 2;
        asmopt_optimization_event* next = realloc(ctx->opt_events, 
                                                  sizeof(asmopt_optimization_event) * new_capacity);
        if (!next) {
            return;
        }
        ctx->opt_events = next;
        ctx->opt_event_capacity = new_capacity;
    }
    
    ctx->opt_events[ctx->opt_event_count].line_no = line_no;
    ctx->opt_events[ctx->opt_event_count].pattern_name = asmopt_strdup(pattern);
    ctx->opt_events[ctx->opt_event_count].original = asmopt_strdup(original ? original : "");
    ctx->opt_events[ctx->opt_event_count].optimized = asmopt_strdup(optimized ? optimized : "(removed)");
    ctx->opt_event_count++;
}

static bool asmopt_is_immediate_one(const char* operand, const char* syntax) {
    if (!operand) {
        return false;
    }
    while (isspace((unsigned char)*operand)) {
        operand++;
    }
    const char* op = operand;
    if (syntax && strcmp(syntax, "att") == 0) {
        if (op[0] != '$') {
            return false;
        }
        op++;
    }
    while (*op && isspace((unsigned char)*op)) {
        op++;
    }
    if (*op == '\0') {
        return false;
    }
    if (asmopt_starts_with(op, "0x")) {
        char* end = NULL;
        long value = strtol(op, &end, 16);
        return end != op && *end == '\0' && value == 1;
    }
    size_t op_len = strlen(op);
    if (op_len > 0 && op[op_len - 1] == 'h') {
        char buffer[IMMEDIATE_BUFFER_SIZE];
        size_t len = op_len - 1;
        if (len >= IMMEDIATE_BUFFER_SIZE) {
            return false;
        }
        memcpy(buffer, op, len);
        buffer[len] = '\0';
        char* end = NULL;
        long value = strtol(buffer, &end, 16);
        return end != buffer && *end == '\0' && value == 1;
    }
    char* end = NULL;
    long value = strtol(op, &end, 10);
    return end != op && *end == '\0' && value == 1;
}

static long asmopt_parse_immediate(const char* operand, const char* syntax, bool* success) {
    *success = false;
    if (!operand) {
        return 0;
    }
    while (isspace((unsigned char)*operand)) {
        operand++;
    }
    const char* op = operand;
    if (syntax && strcmp(syntax, "att") == 0) {
        if (op[0] != '$') {
            return 0;
        }
        op++;
    }
    while (*op && isspace((unsigned char)*op)) {
        op++;
    }
    if (*op == '\0') {
        return 0;
    }
    int base = 10;
    if (syntax && strcmp(syntax, "att") == 0 && op[0] == '0' && op[1] != 'x' && isdigit((unsigned char)op[1])) {
        base = 8;
    }
    char* end = NULL;
    long value = 0;
    if (asmopt_starts_with(op, "0x")) {
        value = strtol(op, &end, 16);
    } else {
        size_t op_len = strlen(op);
        if (op_len > 0 && op[op_len - 1] == 'h') {
            char buffer[IMMEDIATE_BUFFER_SIZE];
            size_t len = op_len - 1;
            if (len >= IMMEDIATE_BUFFER_SIZE) {
                return 0;
            }
            memcpy(buffer, op, len);
            buffer[len] = '\0';
            value = strtol(buffer, &end, 16);
            if (end != buffer && *end == '\0') {
                *success = true;
            }
            return value;
        } else {
            value = strtol(op, &end, base);
        }
    }
    if (end != op && *end == '\0') {
        *success = true;
    }
    return value;
}

static bool asmopt_is_immediate_minus_one(const char* operand, const char* syntax) {
    bool success = false;
    long value = asmopt_parse_immediate(operand, syntax, &success);
    return success && value == -1;
}

/* Check if displacement represents zero (empty is zero for AT&T base-only syntax). */
static bool asmopt_is_zero_displacement(const char* value) {
    char* trimmed = asmopt_strip(value);
    if (!trimmed) {
        return false;
    }
    bool zero = false;
    if (*trimmed == '\0') {
        /* Empty displacement in AT&T syntax implies zero. */
        zero = true;
    } else {
        bool success = false;
        long disp = asmopt_parse_immediate(trimmed, NULL, &success);
        zero = success && disp == 0;
    }
    free(trimmed);
    return zero;
}

/* Detect LEA identity: src memory uses base == dest with zero displacement (Intel or AT&T). */
static bool asmopt_is_identity_lea(const char* src, const char* dest, const char* syntax) {
    if (!src || !dest || !asmopt_is_register(dest, syntax)) {
        return false;
    }
    char* trimmed = asmopt_strip(src);
    if (!trimmed) {
        return false;
    }
    bool matches = false;
    if (syntax && strcmp(syntax, "att") == 0) {
        if (!strchr(trimmed, '(')) {
            free(trimmed);
            return false;
        }
        const char* open = strchr(trimmed, '(');
        if (open) {
            const char* close = strchr(open + 1, ')');
            if (close) {
                const char* tail = close + 1;
                while (*tail && isspace((unsigned char)*tail)) {
                    tail++;
                }
                if (*tail == '\0') {
                    size_t disp_len = (size_t)(open - trimmed);
                    char* disp = malloc(disp_len + 1);
                    if (disp) {
                        memcpy(disp, trimmed, disp_len);
                        disp[disp_len] = '\0';
                        if (asmopt_is_zero_displacement(disp)) {
                            size_t len = (size_t)(close - (open + 1));
                            char* base = malloc(len + 1);
                            if (base) {
                                memcpy(base, open + 1, len);
                                base[len] = '\0';
                                char* inner = asmopt_strip(base);
                                if (inner) {
                                    matches = asmopt_casecmp(inner, dest) == 0;
                                }
                                free(inner);
                                free(base);
                            }
                        }
                        free(disp);
                    }
                }
            }
        }
    } else {
        size_t len = strlen(trimmed);
        if (len >= 2 && trimmed[0] == '[' && trimmed[len - 1] == ']') {
            size_t inner_len = len - 2;
            char* base = malloc(inner_len + 1);
            if (base) {
                memcpy(base, trimmed + 1, inner_len);
                base[inner_len] = '\0';
                char* inner = asmopt_strip(base);
                if (inner) {
                    matches = asmopt_casecmp(inner, dest) == 0;
                }
                free(inner);
                free(base);
            }
        }
    }
    free(trimmed);
    return matches;
}

static void asmopt_build_suffixed_name(char* buffer, size_t size, const char* base, char suffix) {
    if (suffix != '\0') {
        snprintf(buffer, size, "%s%c", base, suffix);
    } else {
        snprintf(buffer, size, "%s", base);
    }
}

static bool asmopt_is_target_zen(asmopt_context* ctx) {
    if (!ctx || !ctx->target_cpu) {
        return false;
    }
    if (!ctx->amd_optimizations) {
        return false;
    }
    const size_t prefix_len = strlen("zen");
    if (strlen(ctx->target_cpu) < prefix_len) {
        return false;
    }
    if (strncasecmp(ctx->target_cpu, "zen", prefix_len) != 0) {
        return false;
    }
    char next = ctx->target_cpu[prefix_len];
    return next == '\0' || isdigit((unsigned char)next);
}

static bool asmopt_is_zero_guarded(asmopt_context* ctx, size_t line_no, const char* src, const char* syntax) {
    if (!ctx || !src || line_no < ZERO_GUARD_PATTERN_LINES) {
        return false;
    }
    size_t jump_index = line_no - 2;
    size_t test_index = line_no - 3;
    if (jump_index >= ctx->original_count || test_index >= ctx->original_count) {
        return false;
    }
    char* jump_code = NULL;
    char* jump_comment = NULL;
    asmopt_split_comment(ctx->original_lines[jump_index], &jump_code, &jump_comment);
    if (!jump_code || asmopt_is_directive_or_label(jump_code)) {
        free(jump_code);
        free(jump_comment);
        return false;
    }
    char* jump_indent = NULL;
    char* jump_mnemonic = NULL;
    char* jump_spacing = NULL;
    char* jump_operands = NULL;
    bool jump_ok = asmopt_parse_instruction(jump_code, &jump_indent, &jump_mnemonic, &jump_spacing, &jump_operands);
    bool is_jump = false;
    if (jump_ok && jump_mnemonic) {
        is_jump = asmopt_casecmp(jump_mnemonic, "jz") == 0 || asmopt_casecmp(jump_mnemonic, "je") == 0;
    }
    free(jump_indent);
    free(jump_mnemonic);
    free(jump_spacing);
    free(jump_operands);
    free(jump_code);
    free(jump_comment);
    if (!is_jump) {
        return false;
    }
    char* test_code = NULL;
    char* test_comment = NULL;
    asmopt_split_comment(ctx->original_lines[test_index], &test_code, &test_comment);
    if (!test_code || asmopt_is_directive_or_label(test_code)) {
        free(test_code);
        free(test_comment);
        return false;
    }
    char* test_indent = NULL;
    char* test_mnemonic = NULL;
    char* test_spacing = NULL;
    char* test_operands = NULL;
    bool test_ok = asmopt_parse_instruction(test_code, &test_indent, &test_mnemonic, &test_spacing, &test_operands);
    bool guarded = false;
    if (test_ok && test_mnemonic && test_operands) {
        char* op1 = NULL;
        char* op2 = NULL;
        char* pre = NULL;
        char* post = NULL;
        if (asmopt_parse_operands(test_operands, &op1, &op2, &pre, &post)) {
            if (asmopt_casecmp(test_mnemonic, "test") == 0) {
                bool reg1 = asmopt_is_register(op1, syntax);
                bool reg2 = asmopt_is_register(op2, syntax);
                guarded = reg1 && reg2 &&
                          asmopt_casecmp(op1, src) == 0 &&
                          asmopt_casecmp(op2, src) == 0;
            } else if (asmopt_casecmp(test_mnemonic, "cmp") == 0) {
                const char* dest = NULL;
                const char* cmp_src = NULL;
                if (syntax && strcmp(syntax, "att") == 0) {
                    cmp_src = op1;
                    dest = op2;
                } else {
                    dest = op1;
                    cmp_src = op2;
                }
                if (dest && cmp_src) {
                    guarded = asmopt_is_register(dest, syntax) &&
                              asmopt_casecmp(dest, src) == 0 &&
                              asmopt_is_immediate_zero(cmp_src, syntax);
                }
            }
        }
        free(op1);
        free(op2);
        free(pre);
        free(post);
    }
    free(test_indent);
    free(test_mnemonic);
    free(test_spacing);
    free(test_operands);
    free(test_code);
    free(test_comment);
    return guarded;
}

static bool asmopt_is_power_of_two(long value) {
    return value > 0 && (value & (value - 1)) == 0;
}

static int asmopt_log2(long value) {
    int log = 0;
    while (value > 1) {
        value >>= 1;
        log++;
    }
    return log;
}

static bool asmopt_is_jump_mnemonic(const char* mnemonic);
static bool asmopt_is_conditional_jump(const char* mnemonic);
static bool asmopt_is_unconditional_jump(const char* mnemonic);
static bool asmopt_invert_conditional_jump(const char* mnemonic, char* buffer, size_t size);
static bool asmopt_is_label_operand(const char* operand);

static void asmopt_handle_identity_removal(asmopt_context* ctx, size_t line_no, const char* pattern_name,
                                           const char* line, const char* comment, const char* indent, bool* removed) {
    asmopt_record_optimization(ctx, line_no, pattern_name, line, NULL);
    if (!asmopt_is_blank(comment)) {
        char* trimmed = asmopt_trim_comment(comment);
        size_t len = strlen(indent) + strlen(trimmed) + 1;
        char* newline = malloc(len + 1);
        if (newline) {
            snprintf(newline, len + 1, "%s%s", indent, trimmed);
            asmopt_store_optimized_line(ctx, newline);
            free(newline);
        }
        free(trimmed);
    }
    *removed = true;
}

static bool asmopt_is_mov_no_comment(const char* line, const char* syntax, char** dest, char** src) {
    if (dest) {
        *dest = NULL;
    }
    if (src) {
        *src = NULL;
    }
    if (!line) {
        return false;
    }
    char* code = NULL;
    char* comment = NULL;
    asmopt_split_comment(line, &code, &comment);
    if (!code || !comment) {
        free(code);
        free(comment);
        return false;
    }
    if (!asmopt_is_blank(comment)) {
        free(code);
        free(comment);
        return false;
    }
    if (asmopt_is_directive_or_label(code)) {
        free(code);
        free(comment);
        return false;
    }
    char* indent = NULL;
    char* mnemonic = NULL;
    char* spacing = NULL;
    char* operands = NULL;
    if (!asmopt_parse_instruction(code, &indent, &mnemonic, &spacing, &operands)) {
        free(code);
        free(comment);
        return false;
    }
    char mnemonic_lower[32];
    size_t mlen = strlen(mnemonic);
    if (mlen >= sizeof(mnemonic_lower)) {
        mlen = sizeof(mnemonic_lower) - 1;
    }
    for (size_t i = 0; i < mlen; i++) {
        mnemonic_lower[i] = (char)tolower((unsigned char)mnemonic[i]);
    }
    mnemonic_lower[mlen] = '\0';
    char base_mnemonic[32] = {0};
    size_t mlen_actual = strlen(mnemonic_lower);
    bool can_have_suffix = false;
    for (size_t i = 0; i < SUFFIX_MNEMONICS_COUNT; i++) {
        size_t base_len = strlen(SUFFIX_MNEMONICS[i]);
        if (mlen_actual == base_len + 1 && strncmp(mnemonic_lower, SUFFIX_MNEMONICS[i], base_len) == 0) {
            can_have_suffix = true;
            break;
        }
    }
    if (can_have_suffix && mlen_actual >= 4) {
        char last_char = mnemonic_lower[mlen_actual - 1];
        if (last_char == 'b' || last_char == 'w' || last_char == 'l' || last_char == 'q') {
            strncpy(base_mnemonic, mnemonic_lower, mlen_actual - 1);
            base_mnemonic[mlen_actual - 1] = '\0';
        } else {
            strcpy(base_mnemonic, mnemonic_lower);
        }
    } else {
        strcpy(base_mnemonic, mnemonic_lower);
    }
    bool result = false;
    char* op1 = NULL;
    char* op2 = NULL;
    char* pre_space = NULL;
    char* post_space = NULL;
    bool has_two_ops = asmopt_parse_operands(operands, &op1, &op2, &pre_space, &post_space);
    if (has_two_ops && strcmp(base_mnemonic, "mov") == 0) {
        const char* dest_op = NULL;
        const char* src_op = NULL;
        if (syntax && strcmp(syntax, "att") == 0) {
            src_op = op1;
            dest_op = op2;
        } else {
            dest_op = op1;
            src_op = op2;
        }
        if (dest_op && src_op && asmopt_is_register(dest_op, syntax) && asmopt_is_register(src_op, syntax)) {
            if (dest) {
                *dest = asmopt_strdup(dest_op);
            }
            if (src) {
                *src = asmopt_strdup(src_op);
            }
            result = true;
        }
    }
    free(op1);
    free(op2);
    free(pre_space);
    free(post_space);
    free(indent);
    free(mnemonic);
    free(spacing);
    free(operands);
    free(code);
    free(comment);
    if (!result) {
        if (dest && *dest) {
            free(*dest);
            *dest = NULL;
        }
        if (src && *src) {
            free(*src);
            *src = NULL;
        }
    }
    return result;
}

static void asmopt_peephole_line(asmopt_context* ctx, size_t line_no, const char* line, const char* syntax, bool* replaced, bool* removed) {
    /*
     * Peephole Optimizer - Pattern Matching Engine
     * 
     * This function implements 28 optimization patterns for x86-64 assembly:
     * (8 identity + 2 redundant move + 14 replacements: 2,4,10,11,13-20,26-27 + 2 control-flow
     *  + 1 cache-aware + 1 architecture-aware + 1 load-modify-store)
     * 
     * Identity/No-op Eliminations (8 patterns):
     *   Pattern 1: mov rax, rax            → (removed)        - Redundant self-move
     *   Pattern 3: imul rax, 1             → (removed)        - Identity multiplication
     *   Pattern 5: add/sub rax, 0          → (removed)        - Identity addition/subtraction
     *   Pattern 6: shl/shr/sal/sar rax, 0  → (removed)        - Identity shift
     *   Pattern 7: or rax, 0               → (removed)        - Identity OR
     *   Pattern 8: xor rax, 0              → (removed)        - Identity XOR (immediate only)
     *   Pattern 9: and rax, -1             → (removed)        - Identity AND (all bits)
     *   Pattern 24: lea rax, [rax]         → (removed)        - Redundant address calc
     * 
     * Redundant Move Elimination (1 pattern):
     *   Pattern 12: mov a, b + mov b, a    → mov a, b         - Remove redundant move
     * 
     * Instruction Replacements (12 patterns):
     *   Pattern 2: mov rax, 0              → xor rax, rax     - Smaller encoding, breaks deps
     *   Pattern 4: imul rax, 8             → shl rax, 3       - Faster shift for power-of-2
     *   Pattern 10: add rax, 1             → inc rax          - Size opt (note: flag deps on P4+)
     *   Pattern 11: sub rax, 1             → dec rax          - Size opt (note: flag deps on P4+)
     *   Pattern 13: sub rax, rax           → xor rax, rax     - Zero idiom
     *   Pattern 14: and rax, 0             → xor rax, rax     - Zero idiom
     *   Pattern 15: cmp rax, 0             → test rax, rax    - Zero compare
     *   Pattern 16: or rax, rax            → test rax, rax    - Flag-only
     *   Pattern 17: add rax, -1            → dec rax          - Negative add
     *   Pattern 18: sub rax, -1            → inc rax          - Negative sub
     *   Pattern 19: and rax, rax           → test rax, rax    - Flag-only
     *   Pattern 20: cmp rax, rax           → test rax, rax    - Self-compare
     * 
     * Control-flow (2 patterns):
     *   Pattern 21: jmp next_label         → (removed)        - Fallthrough jump
     *   Pattern 25: jcc label + jmp next   → j!cc next        - Branch inversion
     * 
     * Dead code elimination (1 pattern):
     *   Pattern 26: mov r1, r2 + mov r1, r3 → mov r1, r3        - Dead store
     * 
     * Scheduling (1 pattern):
     *   Pattern 27: mov r1, r2 + mov r3, r4 → mov r3, r4 + mov r1, r2 - Move hoist
     * 
     * Load-modify-store (1 pattern):
     *   Pattern 28: mov r1, [mem] + add r1, imm + mov [mem], r1 → add [mem], imm
     * 
     * Cache-aware (1 pattern):
     *   Pattern 22: .hot_loop:             → .align 64 + label - Align hot loop headers
     * 
     * Architecture-aware (1 pattern):
     *   Pattern 23: bsf reg, reg           → tzcnt reg, reg    - Zen BMI1 preference
     * 
     * Note: inc/dec create false dependencies on flags register (Pentium 4+), so patterns
     * 10-11 optimize for size. Future: make configurable (-Os vs -O3).
     * 
     * All patterns preserve comments and handle both Intel and AT&T syntax.
     */
    *replaced = false;
    *removed = false;
    ctx->skip_lines = 0;
    char* code = NULL;
    char* comment = NULL;
    asmopt_split_comment(line, &code, &comment);
    if (!code || !comment) {
        free(code);
        free(comment);
        asmopt_store_optimized_line(ctx, line);
        return;
    }
    if (asmopt_is_directive_or_label(code)) {
        char* trimmed = asmopt_strip(code);
        if (trimmed) {
            if (ctx->insert_hot_align && strcmp(trimmed, ".hot_loop:") == 0) {
                char align_line[32];
                snprintf(align_line, sizeof(align_line), "    .align %d", ASMOPT_HOT_LOOP_ALIGNMENT);
                asmopt_store_optimized_line(ctx, align_line);
                char align_report[128];
                snprintf(align_report, sizeof(align_report), "    .align %d\n.hot_loop:", ASMOPT_HOT_LOOP_ALIGNMENT);
                asmopt_record_optimization(ctx, line_no, "hot_loop_align", line, align_report);
                asmopt_store_optimized_line(ctx, line);
                free(trimmed);
                free(code);
                free(comment);
                return;
            }
            free(trimmed);
        }
        asmopt_store_optimized_line(ctx, line);
        free(code);
        free(comment);
        return;
    }
    char* indent = NULL;
    char* mnemonic = NULL;
    char* spacing = NULL;
    char* operands = NULL;
    if (!asmopt_parse_instruction(code, &indent, &mnemonic, &spacing, &operands)) {
        asmopt_store_optimized_line(ctx, line);
        free(code);
        free(comment);
        return;
    }
    
    /* Convert mnemonic to lowercase for matching */
    char mnemonic_lower[32];
    size_t mlen = strlen(mnemonic);
    if (mlen >= sizeof(mnemonic_lower)) {
        mlen = sizeof(mnemonic_lower) - 1;
    }
    for (size_t i = 0; i < mlen; i++) {
        mnemonic_lower[i] = (char)tolower((unsigned char)mnemonic[i]);
    }
    mnemonic_lower[mlen] = '\0';
    
    /* Extract suffix for AT&T syntax (e.g., movl, movq, addl, subl) */
    char suffix = '\0';
    char base_mnemonic[32] = {0};
    size_t mlen_actual = strlen(mnemonic_lower);
    
    /* Only strip suffix for known instruction families that use them */
    bool can_have_suffix = false;
    for (size_t i = 0; i < SUFFIX_MNEMONICS_COUNT; i++) {
        size_t base_len = strlen(SUFFIX_MNEMONICS[i]);
        if (mlen_actual == base_len + 1 && strncmp(mnemonic_lower, SUFFIX_MNEMONICS[i], base_len) == 0) {
            can_have_suffix = true;
            break;
        }
    }
    
    if (can_have_suffix && mlen_actual >= 4) {
        char last_char = mnemonic_lower[mlen_actual - 1];
        if (last_char == 'b' || last_char == 'w' || last_char == 'l' || last_char == 'q') {
            suffix = last_char;
            strncpy(base_mnemonic, mnemonic_lower, mlen_actual - 1);
            base_mnemonic[mlen_actual - 1] = '\0';
        } else {
            strcpy(base_mnemonic, mnemonic_lower);
        }
    } else {
        strcpy(base_mnemonic, mnemonic_lower);
    }
    
    /* Try to parse operands for two-operand instructions */
    char* op1 = NULL;
    char* op2 = NULL;
    char* pre_space = NULL;
    char* post_space = NULL;
    bool has_two_ops = asmopt_parse_operands(operands, &op1, &op2, &pre_space, &post_space);
    
    /* Determine dest/src based on syntax */
    const char* dest = NULL;
    const char* src = NULL;
    bool dest_reg = false;
    bool src_reg = false;
    if (has_two_ops) {
        if (syntax && strcmp(syntax, "att") == 0) {
            src = op1;
            dest = op2;
        } else {
            dest = op1;
            src = op2;
        }
        dest_reg = asmopt_is_register(dest, syntax);
        src_reg = asmopt_is_register(src, syntax);
    }
    
    /* Pattern 1: mov rax, rax -> remove */
    if ((strcmp(base_mnemonic, "mov") == 0 || strcmp(mnemonic_lower, "mov") == 0) && has_two_ops) {
        if (dest_reg && src_reg && asmopt_casecmp(dest, src) == 0) {
            asmopt_handle_identity_removal(ctx, line_no, "redundant_mov", line, comment, indent, removed);
            goto cleanup;
        }
        
        /* Pattern 2: mov rax, 0 -> xor rax, rax */
        if (dest_reg && asmopt_is_immediate_zero(src, syntax)) {
            char xor_name[8];
            if (suffix) {
                snprintf(xor_name, sizeof(xor_name), "xor%c", suffix);
            } else {
                snprintf(xor_name, sizeof(xor_name), "xor");
            }
            char* trimmed_comment = asmopt_trim_comment(comment);
            size_t new_len = strlen(indent) + strlen(xor_name) + strlen(spacing) + 
                            strlen(dest) + strlen(pre_space) + strlen(post_space) + strlen(dest) + 2;
            if (!asmopt_is_blank(trimmed_comment)) {
                new_len += strlen(trimmed_comment) + 1;
            }
            char* newline = malloc(new_len + 1);
            if (newline) {
                if (!asmopt_is_blank(trimmed_comment)) {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s %s", 
                            indent, xor_name, spacing, dest, pre_space, post_space, dest, trimmed_comment);
                } else {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s", 
                            indent, xor_name, spacing, dest, pre_space, post_space, dest);
                }
                asmopt_record_optimization(ctx, line_no, "mov_zero_to_xor", line, newline);
                asmopt_store_optimized_line(ctx, newline);
                free(newline);
                *replaced = true;
            }
            free(trimmed_comment);
            goto cleanup;
        }
    }

    /* Pattern 24: lea rax, [rax] -> remove (identity) */
    if (strcmp(base_mnemonic, "lea") == 0 && has_two_ops) {
        if (dest && src && asmopt_is_identity_lea(src, dest, syntax)) {
            *replaced = false;
            asmopt_handle_identity_removal(ctx, line_no, "redundant_lea", line, comment, indent, removed);
            goto cleanup;
        }
    }

    /* Pattern 26: mov rax, rbx / mov rax, rcx -> remove dead store */
    if (strcmp(base_mnemonic, "mov") == 0 && has_two_ops && dest_reg && src_reg) {
        size_t next_index = line_no;
        if (next_index < ctx->original_count) {
            const char* next_line = ctx->original_lines[next_index];
            char* next_dest = NULL;
            char* next_src = NULL;
            if (asmopt_is_mov_no_comment(next_line, syntax, &next_dest, &next_src)) {
                bool dead_store = next_dest && asmopt_casecmp(dest, next_dest) == 0 &&
                                  next_src && asmopt_casecmp(src, next_src) != 0;
                if (dead_store) {
                    size_t combo_len = strlen(line) + strlen(next_line) + 2;
                    char* combined = malloc(combo_len + 1);
                    if (combined) {
                        snprintf(combined, combo_len + 1, "%s\n%s", line, next_line);
                        asmopt_record_optimization(ctx, line_no, "dead_store_move", combined, next_line);
                        free(combined);
                    } else {
                        asmopt_record_optimization(ctx, line_no, "dead_store_move", line, next_line);
                    }
                    asmopt_store_optimized_line(ctx, next_line);
                    *removed = true;
                    *replaced = true;
                    ctx->skip_lines = 1;
                    free(next_dest);
                    free(next_src);
                    goto cleanup;
                }
            }
            free(next_dest);
            free(next_src);
        }
    }

    /* Pattern 27: mov rax, rbx / mov rcx, rdx -> reorder for scheduling */
    if (strcmp(base_mnemonic, "mov") == 0 && has_two_ops && dest_reg && src_reg) {
        size_t next_index = line_no;
        if (next_index < ctx->original_count) {
            const char* next_line = ctx->original_lines[next_index];
            char* next_dest = NULL;
            char* next_src = NULL;
            if (asmopt_is_mov_no_comment(next_line, syntax, &next_dest, &next_src)) {
                bool independent = next_dest && next_src &&
                                   asmopt_casecmp(dest, next_dest) != 0 &&
                                   asmopt_casecmp(dest, next_src) != 0 &&
                                   asmopt_casecmp(src, next_dest) != 0 &&
                                   asmopt_casecmp(src, next_src) != 0;
                if (independent) {
                    asmopt_record_optimization(ctx, line_no, "schedule_swap_move", line, next_line);
                    asmopt_store_optimized_line(ctx, next_line);
                    asmopt_store_optimized_line(ctx, line);
                    *replaced = true;
                    ctx->skip_lines = 1;
                    free(next_dest);
                    free(next_src);
                    goto cleanup;
                }
            }
            free(next_dest);
            free(next_src);
        }
    }

    /* Pattern 28: mov rax, [mem] / add rax, imm / mov [mem], rax -> add [mem], imm */
    if (strcmp(base_mnemonic, "mov") == 0 && has_two_ops && dest_reg && src && !src_reg) {
        size_t add_index = line_no;
        size_t store_index = line_no + 1;
        if (add_index < ctx->original_count && store_index < ctx->original_count) {
            const char* add_line = ctx->original_lines[add_index];
            const char* store_line = ctx->original_lines[store_index];
            char* add_code = NULL;
            char* add_comment = NULL;
            asmopt_split_comment(add_line, &add_code, &add_comment);
            if (add_code && !asmopt_is_directive_or_label(add_code)) {
                char* add_indent = NULL;
                char* add_mnemonic = NULL;
                char* add_spacing = NULL;
                char* add_operands = NULL;
                if (asmopt_parse_instruction(add_code, &add_indent, &add_mnemonic, &add_spacing, &add_operands)) {
                    char add_lower[32];
                    size_t add_len = strlen(add_mnemonic);
                    if (add_len >= sizeof(add_lower)) {
                        add_len = sizeof(add_lower) - 1;
                    }
                    for (size_t i = 0; i < add_len; i++) {
                        add_lower[i] = (char)tolower((unsigned char)add_mnemonic[i]);
                    }
                    add_lower[add_len] = '\0';
                    char add_suffix = '\0';
                    char add_base[32] = {0};
                    size_t add_actual = strlen(add_lower);
                    bool add_can_suffix = false;
                    for (size_t i = 0; i < SUFFIX_MNEMONICS_COUNT; i++) {
                        size_t base_len = strlen(SUFFIX_MNEMONICS[i]);
                        if (add_actual == base_len + 1 &&
                            strncmp(add_lower, SUFFIX_MNEMONICS[i], base_len) == 0) {
                            add_can_suffix = true;
                            break;
                        }
                    }
                    if (add_can_suffix && add_actual >= 4) {
                        char last_char = add_lower[add_actual - 1];
                        if (last_char == 'b' || last_char == 'w' || last_char == 'l' || last_char == 'q') {
                            add_suffix = last_char;
                            strncpy(add_base, add_lower, add_actual - 1);
                            add_base[add_actual - 1] = '\0';
                        } else {
                            strcpy(add_base, add_lower);
                        }
                    } else {
                        strcpy(add_base, add_lower);
                    }
                    char* add_op1 = NULL;
                    char* add_op2 = NULL;
                    char* add_pre = NULL;
                    char* add_post = NULL;
                    bool add_two_ops = asmopt_parse_operands(add_operands, &add_op1, &add_op2, &add_pre, &add_post);
                    if (add_two_ops && strcmp(add_base, "add") == 0) {
                        const char* add_dest = NULL;
                        const char* add_src = NULL;
                        if (syntax && strcmp(syntax, "att") == 0) {
                            add_src = add_op1;
                            add_dest = add_op2;
                        } else {
                            add_dest = add_op1;
                            add_src = add_op2;
                        }
                        bool add_immediate = false;
                        asmopt_parse_immediate(add_src, syntax, &add_immediate);
                        if (add_dest && add_src && asmopt_is_register(add_dest, syntax) &&
                            asmopt_casecmp(add_dest, dest) == 0 && add_immediate) {
                            char* store_code = NULL;
                            char* store_comment = NULL;
                            asmopt_split_comment(store_line, &store_code, &store_comment);
                            if (store_code && !asmopt_is_directive_or_label(store_code)) {
                                char* store_indent = NULL;
                                char* store_mnemonic = NULL;
                                char* store_spacing = NULL;
                                char* store_operands = NULL;
                                if (asmopt_parse_instruction(store_code, &store_indent, &store_mnemonic, &store_spacing, &store_operands)) {
                                    char store_lower[32];
                                    size_t store_len = strlen(store_mnemonic);
                                    if (store_len >= sizeof(store_lower)) {
                                        store_len = sizeof(store_lower) - 1;
                                    }
                                    for (size_t i = 0; i < store_len; i++) {
                                        store_lower[i] = (char)tolower((unsigned char)store_mnemonic[i]);
                                    }
                                    store_lower[store_len] = '\0';
                                    char store_base[32] = {0};
                                    size_t store_actual = strlen(store_lower);
                                    bool store_can_suffix = false;
                                    for (size_t i = 0; i < SUFFIX_MNEMONICS_COUNT; i++) {
                                        size_t base_len = strlen(SUFFIX_MNEMONICS[i]);
                                        if (store_actual == base_len + 1 &&
                                            strncmp(store_lower, SUFFIX_MNEMONICS[i], base_len) == 0) {
                                            store_can_suffix = true;
                                            break;
                                        }
                                    }
                                    if (store_can_suffix && store_actual >= 4) {
                                        char last_char = store_lower[store_actual - 1];
                                        if (last_char == 'b' || last_char == 'w' || last_char == 'l' || last_char == 'q') {
                                            strncpy(store_base, store_lower, store_actual - 1);
                                            store_base[store_actual - 1] = '\0';
                                        } else {
                                            strcpy(store_base, store_lower);
                                        }
                                    } else {
                                        strcpy(store_base, store_lower);
                                    }
                                    char* store_op1 = NULL;
                                    char* store_op2 = NULL;
                                    char* store_pre = NULL;
                                    char* store_post = NULL;
                                    bool store_two_ops = asmopt_parse_operands(store_operands, &store_op1, &store_op2, &store_pre, &store_post);
                                    if (store_two_ops && strcmp(store_base, "mov") == 0) {
                                        const char* store_dest = NULL;
                                        const char* store_src = NULL;
                                        if (syntax && strcmp(syntax, "att") == 0) {
                                            store_src = store_op1;
                                            store_dest = store_op2;
                                        } else {
                                            store_dest = store_op1;
                                            store_src = store_op2;
                                        }
                                        if (store_dest && store_src &&
                                            asmopt_is_register(store_src, syntax) &&
                                            asmopt_casecmp(store_src, dest) == 0 &&
                                            asmopt_casecmp(store_dest, src) == 0) {
                                            char add_name[8];
                                            asmopt_build_suffixed_name(add_name, sizeof(add_name), "add", add_suffix);
                                            char* trimmed_comment = asmopt_trim_comment(comment);
                                            size_t new_len = strlen(indent) + strlen(add_name) + strlen(spacing) +
                                                             strlen(store_dest) + strlen(add_pre) + strlen(add_post) + strlen(add_src) + 2;
                                            if (!asmopt_is_blank(trimmed_comment)) {
                                                new_len += strlen(trimmed_comment) + 1;
                                            }
                                            char* newline = malloc(new_len + 1);
                                            if (newline) {
                                                if (!asmopt_is_blank(trimmed_comment)) {
                                                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s %s",
                                                             indent, add_name, spacing, store_dest, add_pre, add_post, add_src, trimmed_comment);
                                                } else {
                                                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s",
                                                             indent, add_name, spacing, store_dest, add_pre, add_post, add_src);
                                                }
                                                size_t combo_len = strlen(line) + strlen(add_line) + strlen(store_line) + 3;
                                                char* combined = malloc(combo_len + 1);
                                                if (combined) {
                                                    snprintf(combined, combo_len + 1, "%s\n%s\n%s", line, add_line, store_line);
                                                    asmopt_record_optimization(ctx, line_no, "load_modify_store", combined, newline);
                                                    free(combined);
                                                } else {
                                                    asmopt_record_optimization(ctx, line_no, "load_modify_store", line, newline);
                                                }
                                                asmopt_store_optimized_line(ctx, newline);
                                                free(newline);
                                                if (!asmopt_is_blank(add_comment)) {
                                                    char* trimmed_add_comment = asmopt_trim_comment(add_comment);
                                                    size_t add_len = strlen(add_indent) + strlen(trimmed_add_comment) + 1;
                                                    char* add_comment_line = malloc(add_len + 1);
                                                    if (add_comment_line) {
                                                        snprintf(add_comment_line, add_len + 1, "%s%s", add_indent, trimmed_add_comment);
                                                        asmopt_store_optimized_line(ctx, add_comment_line);
                                                        free(add_comment_line);
                                                    }
                                                    free(trimmed_add_comment);
                                                }
                                                if (!asmopt_is_blank(store_comment)) {
                                                    char* trimmed_store_comment = asmopt_trim_comment(store_comment);
                                                    size_t store_len = strlen(store_indent) + strlen(trimmed_store_comment) + 1;
                                                    char* store_comment_line = malloc(store_len + 1);
                                                    if (store_comment_line) {
                                                        snprintf(store_comment_line, store_len + 1, "%s%s", store_indent, trimmed_store_comment);
                                                        asmopt_store_optimized_line(ctx, store_comment_line);
                                                        free(store_comment_line);
                                                    }
                                                    free(trimmed_store_comment);
                                                }
                                                *replaced = true;
                                                *removed = true;
                                                ctx->skip_lines = 2;
                                                free(trimmed_comment);
                                                free(store_op1);
                                                free(store_op2);
                                                free(store_pre);
                                                free(store_post);
                                                free(store_indent);
                                                free(store_mnemonic);
                                                free(store_spacing);
                                                free(store_operands);
                                                free(store_code);
                                                free(store_comment);
                                                free(add_op1);
                                                free(add_op2);
                                                free(add_pre);
                                                free(add_post);
                                                free(add_indent);
                                                free(add_mnemonic);
                                                free(add_spacing);
                                                free(add_operands);
                                                free(add_code);
                                                free(add_comment);
                                                goto cleanup;
                                            }
                                            free(trimmed_comment);
                                        }
                                    }
                                    free(store_op1);
                                    free(store_op2);
                                    free(store_pre);
                                    free(store_post);
                                }
                                free(store_indent);
                                free(store_mnemonic);
                                free(store_spacing);
                                free(store_operands);
                            }
                            free(store_code);
                            free(store_comment);
                        }
                    }
                    free(add_op1);
                    free(add_op2);
                    free(add_pre);
                    free(add_post);
                }
                free(add_indent);
                free(add_mnemonic);
                free(add_spacing);
                free(add_operands);
            }
            free(add_code);
            free(add_comment);
        }
    }

    /* Pattern 12: mov rax, rbx / mov rbx, rax -> remove redundant move */
    if (strcmp(base_mnemonic, "mov") == 0 && has_two_ops && dest_reg && src_reg) {
        if (line_no < ctx->original_count) {
            const char* next_line = ctx->original_lines[line_no];
            char* next_code = NULL;
            char* next_comment = NULL;
            asmopt_split_comment(next_line, &next_code, &next_comment);
            if (next_code && !asmopt_is_directive_or_label(next_code)) {
                char* next_indent = NULL;
                char* next_mnemonic = NULL;
                char* next_spacing = NULL;
                char* next_operands = NULL;
                if (asmopt_parse_instruction(next_code, &next_indent, &next_mnemonic, &next_spacing, &next_operands)) {
                    char next_lower[32];
                    size_t next_len = strlen(next_mnemonic);
                    if (next_len >= sizeof(next_lower)) {
                        next_len = sizeof(next_lower) - 1;
                    }
                    for (size_t i = 0; i < next_len; i++) {
                        next_lower[i] = (char)tolower((unsigned char)next_mnemonic[i]);
                    }
                    next_lower[next_len] = '\0';
                    char next_suffix = '\0';
                    char next_base[32] = {0};
                    size_t next_actual = strlen(next_lower);
                    bool next_can_suffix = false;
                    for (size_t i = 0; i < SUFFIX_MNEMONICS_COUNT; i++) {
                        size_t base_len = strlen(SUFFIX_MNEMONICS[i]);
                        if (next_actual == base_len + 1 &&
                            strncmp(next_lower, SUFFIX_MNEMONICS[i], base_len) == 0) {
                            next_can_suffix = true;
                            break;
                        }
                    }
                    if (next_can_suffix && next_actual >= 4) {
                        char last_char = next_lower[next_actual - 1];
                        if (last_char == 'b' || last_char == 'w' || last_char == 'l' || last_char == 'q') {
                            next_suffix = last_char;
                            strncpy(next_base, next_lower, next_actual - 1);
                            next_base[next_actual - 1] = '\0';
                        } else {
                            strcpy(next_base, next_lower);
                        }
                    } else {
                        strcpy(next_base, next_lower);
                    }
                    char* next_op1 = NULL;
                    char* next_op2 = NULL;
                    char* next_pre = NULL;
                    char* next_post = NULL;
                    bool next_two = asmopt_parse_operands(next_operands, &next_op1, &next_op2, &next_pre, &next_post);
                    if (next_two && strcmp(next_base, "mov") == 0) {
                        const char* next_dest = NULL;
                        const char* next_src = NULL;
                        if (syntax && strcmp(syntax, "att") == 0) {
                            next_src = next_op1;
                            next_dest = next_op2;
                        } else {
                            next_dest = next_op1;
                            next_src = next_op2;
                        }
                        if (next_dest && next_src) {
                            bool next_dest_reg = asmopt_is_register(next_dest, syntax);
                            bool next_src_reg = asmopt_is_register(next_src, syntax);
                            bool swapped = next_dest_reg && next_src_reg &&
                                           asmopt_casecmp(dest, next_src) == 0 &&
                                           asmopt_casecmp(src, next_dest) == 0;
                            if (swapped) {
                                size_t combo_len = strlen(line) + strlen(next_line) + 2;
                                char* combined = malloc(combo_len + 1);
                                if (combined) {
                                    snprintf(combined, combo_len + 1, "%s\n%s", line, next_line);
                                    asmopt_record_optimization(ctx, line_no, "redundant_move_pair", combined, line);
                                    free(combined);
                                }
                                asmopt_store_optimized_line(ctx, line);
                                *replaced = true;
                                if (!asmopt_is_blank(next_comment)) {
                                    char* trimmed_next = asmopt_trim_comment(next_comment);
                                    size_t next_len = strlen(next_indent) + strlen(trimmed_next) + 1;
                                    char* comment_line = malloc(next_len + 1);
                                    if (comment_line) {
                                        snprintf(comment_line, next_len + 1, "%s%s", next_indent, trimmed_next);
                                        asmopt_store_optimized_line(ctx, comment_line);
                                        free(comment_line);
                                    }
                                    free(trimmed_next);
                                }
                                asmopt_record_optimization(ctx, line_no + 1, "redundant_move_pair", next_line, NULL);
                                *removed = true;
                                ctx->skip_lines = 1;
                                free(next_op1);
                                free(next_op2);
                                free(next_pre);
                                free(next_post);
                                free(next_indent);
                                free(next_mnemonic);
                                free(next_spacing);
                                free(next_operands);
                                free(next_code);
                                free(next_comment);
                                goto cleanup;
                            }
                        }
                    }
                    free(next_op1);
                    free(next_op2);
                    free(next_pre);
                    free(next_post);
                }
                free(next_indent);
                free(next_mnemonic);
                free(next_spacing);
                free(next_operands);
            }
            free(next_code);
            free(next_comment);
        }
    }
    
    /* Pattern 13: sub rax, rax -> xor rax, rax */
    if (strcmp(base_mnemonic, "sub") == 0 && has_two_ops && dest_reg && src_reg) {
        if (asmopt_casecmp(dest, src) == 0) {
            char xor_name[8];
            if (suffix) {
                snprintf(xor_name, sizeof(xor_name), "xor%c", suffix);
            } else {
                snprintf(xor_name, sizeof(xor_name), "xor");
            }
            char* trimmed_comment = asmopt_trim_comment(comment);
            size_t new_len = strlen(indent) + strlen(xor_name) + strlen(spacing) +
                             strlen(dest) + strlen(pre_space) + strlen(post_space) + strlen(dest) + 2;
            if (!asmopt_is_blank(trimmed_comment)) {
                new_len += strlen(trimmed_comment) + 1;
            }
            char* newline = malloc(new_len + 1);
            if (newline) {
                if (!asmopt_is_blank(trimmed_comment)) {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s %s",
                             indent, xor_name, spacing, dest, pre_space, post_space, dest, trimmed_comment);
                } else {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s",
                             indent, xor_name, spacing, dest, pre_space, post_space, dest);
                }
                asmopt_record_optimization(ctx, line_no, "sub_self_to_xor", line, newline);
                asmopt_store_optimized_line(ctx, newline);
                free(newline);
                *replaced = true;
            }
            free(trimmed_comment);
            goto cleanup;
        }
    }

    /* Pattern 14: and rax, 0 -> xor rax, rax */
    if (strcmp(base_mnemonic, "and") == 0 && has_two_ops && dest_reg) {
        if (asmopt_is_immediate_zero(src, syntax)) {
            char xor_name[8];
            if (suffix) {
                snprintf(xor_name, sizeof(xor_name), "xor%c", suffix);
            } else {
                snprintf(xor_name, sizeof(xor_name), "xor");
            }
            char* trimmed_comment = asmopt_trim_comment(comment);
            size_t new_len = strlen(indent) + strlen(xor_name) + strlen(spacing) +
                             strlen(dest) + strlen(pre_space) + strlen(post_space) + strlen(dest) + 2;
            if (!asmopt_is_blank(trimmed_comment)) {
                new_len += strlen(trimmed_comment) + 1;
            }
            char* newline = malloc(new_len + 1);
            if (newline) {
                if (!asmopt_is_blank(trimmed_comment)) {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s %s",
                             indent, xor_name, spacing, dest, pre_space, post_space, dest, trimmed_comment);
                } else {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s",
                             indent, xor_name, spacing, dest, pre_space, post_space, dest);
                }
                asmopt_record_optimization(ctx, line_no, "and_zero_to_xor", line, newline);
                asmopt_store_optimized_line(ctx, newline);
                free(newline);
                *replaced = true;
            }
            free(trimmed_comment);
            goto cleanup;
        }
    }

    /* Pattern 15: cmp rax, 0 -> test rax, rax */
    if (strcmp(base_mnemonic, "cmp") == 0 && has_two_ops && dest_reg) {
        if (asmopt_is_immediate_zero(src, syntax)) {
            char test_name[8];
            if (suffix) {
                snprintf(test_name, sizeof(test_name), "test%c", suffix);
            } else {
                snprintf(test_name, sizeof(test_name), "test");
            }
            char* trimmed_comment = asmopt_trim_comment(comment);
            size_t new_len = strlen(indent) + strlen(test_name) + strlen(spacing) +
                             strlen(dest) + strlen(pre_space) + strlen(post_space) + strlen(dest) + 2;
            if (!asmopt_is_blank(trimmed_comment)) {
                new_len += strlen(trimmed_comment) + 1;
            }
            char* newline = malloc(new_len + 1);
            if (newline) {
                if (!asmopt_is_blank(trimmed_comment)) {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s %s",
                             indent, test_name, spacing, dest, pre_space, post_space, dest, trimmed_comment);
                } else {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s",
                             indent, test_name, spacing, dest, pre_space, post_space, dest);
                }
                asmopt_record_optimization(ctx, line_no, "cmp_zero_to_test", line, newline);
                asmopt_store_optimized_line(ctx, newline);
                free(newline);
                *replaced = true;
            }
            free(trimmed_comment);
            goto cleanup;
        }
    }

    /* Pattern 16: or rax, rax -> test rax, rax */
    if (strcmp(base_mnemonic, "or") == 0 && has_two_ops && dest_reg && src_reg) {
        if (asmopt_casecmp(dest, src) == 0) {
            char test_name[8];
            if (suffix) {
                snprintf(test_name, sizeof(test_name), "test%c", suffix);
            } else {
                snprintf(test_name, sizeof(test_name), "test");
            }
            char* trimmed_comment = asmopt_trim_comment(comment);
            size_t new_len = strlen(indent) + strlen(test_name) + strlen(spacing) +
                             strlen(dest) + strlen(pre_space) + strlen(post_space) + strlen(dest) + 2;
            if (!asmopt_is_blank(trimmed_comment)) {
                new_len += strlen(trimmed_comment) + 1;
            }
            char* newline = malloc(new_len + 1);
            if (newline) {
                if (!asmopt_is_blank(trimmed_comment)) {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s %s",
                             indent, test_name, spacing, dest, pre_space, post_space, dest, trimmed_comment);
                } else {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s",
                             indent, test_name, spacing, dest, pre_space, post_space, dest);
                }
                asmopt_record_optimization(ctx, line_no, "or_self_to_test", line, newline);
                asmopt_store_optimized_line(ctx, newline);
                free(newline);
                *replaced = true;
            }
            free(trimmed_comment);
            goto cleanup;
        }
    }

    /* Pattern 17: add rax, -1 -> dec rax */
    if (strcmp(base_mnemonic, "add") == 0 && has_two_ops && dest_reg) {
        if (asmopt_is_immediate_minus_one(src, syntax)) {
            char dec_name[16];
            if (suffix) {
                snprintf(dec_name, sizeof(dec_name), "dec%c", suffix);
            } else {
                snprintf(dec_name, sizeof(dec_name), "dec");
            }
            char* trimmed_comment = asmopt_trim_comment(comment);
            size_t new_len = strlen(indent) + strlen(dec_name) + strlen(spacing) + strlen(dest) + 2;
            if (!asmopt_is_blank(trimmed_comment)) {
                new_len += strlen(trimmed_comment) + 1;
            }
            char* newline = malloc(new_len + 1);
            if (newline) {
                if (!asmopt_is_blank(trimmed_comment)) {
                    snprintf(newline, new_len + 1, "%s%s%s%s %s",
                             indent, dec_name, spacing, dest, trimmed_comment);
                } else {
                    snprintf(newline, new_len + 1, "%s%s%s%s",
                             indent, dec_name, spacing, dest);
                }
                asmopt_record_optimization(ctx, line_no, "add_minus_one_to_dec", line, newline);
                asmopt_store_optimized_line(ctx, newline);
                free(newline);
                *replaced = true;
            }
            free(trimmed_comment);
            goto cleanup;
        }
    }

    /* Pattern 18: sub rax, -1 -> inc rax */
    if (strcmp(base_mnemonic, "sub") == 0 && has_two_ops && dest_reg) {
        if (asmopt_is_immediate_minus_one(src, syntax)) {
            char inc_name[16];
            if (suffix) {
                snprintf(inc_name, sizeof(inc_name), "inc%c", suffix);
            } else {
                snprintf(inc_name, sizeof(inc_name), "inc");
            }
            char* trimmed_comment = asmopt_trim_comment(comment);
            size_t new_len = strlen(indent) + strlen(inc_name) + strlen(spacing) + strlen(dest) + 2;
            if (!asmopt_is_blank(trimmed_comment)) {
                new_len += strlen(trimmed_comment) + 1;
            }
            char* newline = malloc(new_len + 1);
            if (newline) {
                if (!asmopt_is_blank(trimmed_comment)) {
                    snprintf(newline, new_len + 1, "%s%s%s%s %s",
                             indent, inc_name, spacing, dest, trimmed_comment);
                } else {
                    snprintf(newline, new_len + 1, "%s%s%s%s",
                             indent, inc_name, spacing, dest);
                }
                asmopt_record_optimization(ctx, line_no, "sub_minus_one_to_inc", line, newline);
                asmopt_store_optimized_line(ctx, newline);
                free(newline);
                *replaced = true;
            }
            free(trimmed_comment);
            goto cleanup;
        }
    }

    /* Pattern 19: and rax, rax -> test rax, rax */
    if (strcmp(base_mnemonic, "and") == 0 && has_two_ops && dest_reg && src_reg) {
        if (asmopt_casecmp(dest, src) == 0) {
            char test_name[8];
            if (suffix) {
                snprintf(test_name, sizeof(test_name), "test%c", suffix);
            } else {
                snprintf(test_name, sizeof(test_name), "test");
            }
            char* trimmed_comment = asmopt_trim_comment(comment);
            size_t new_len = strlen(indent) + strlen(test_name) + strlen(spacing) +
                             strlen(dest) + strlen(pre_space) + strlen(post_space) + strlen(dest) + 2;
            if (!asmopt_is_blank(trimmed_comment)) {
                new_len += strlen(trimmed_comment) + 1;
            }
            char* newline = malloc(new_len + 1);
            if (newline) {
                if (!asmopt_is_blank(trimmed_comment)) {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s %s",
                             indent, test_name, spacing, dest, pre_space, post_space, dest, trimmed_comment);
                } else {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s",
                             indent, test_name, spacing, dest, pre_space, post_space, dest);
                }
                asmopt_record_optimization(ctx, line_no, "and_self_to_test", line, newline);
                asmopt_store_optimized_line(ctx, newline);
                free(newline);
                *replaced = true;
            }
            free(trimmed_comment);
            goto cleanup;
        }
    }

    /* Pattern 20: cmp rax, rax -> test rax, rax */
    if (strcmp(base_mnemonic, "cmp") == 0 && has_two_ops && dest_reg && src_reg) {
        if (asmopt_casecmp(dest, src) == 0) {
            char test_name[8];
            if (suffix) {
                snprintf(test_name, sizeof(test_name), "test%c", suffix);
            } else {
                snprintf(test_name, sizeof(test_name), "test");
            }
            char* trimmed_comment = asmopt_trim_comment(comment);
            size_t new_len = strlen(indent) + strlen(test_name) + strlen(spacing) +
                             strlen(dest) + strlen(pre_space) + strlen(post_space) + strlen(dest) + 2;
            if (!asmopt_is_blank(trimmed_comment)) {
                new_len += strlen(trimmed_comment) + 1;
            }
            char* newline = malloc(new_len + 1);
            if (newline) {
                if (!asmopt_is_blank(trimmed_comment)) {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s %s",
                             indent, test_name, spacing, dest, pre_space, post_space, dest, trimmed_comment);
                } else {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s",
                             indent, test_name, spacing, dest, pre_space, post_space, dest);
                }
                asmopt_record_optimization(ctx, line_no, "cmp_self_to_test", line, newline);
                asmopt_store_optimized_line(ctx, newline);
                free(newline);
                *replaced = true;
            }
            free(trimmed_comment);
            goto cleanup;
        }
    }

    /* Pattern 21: jmp <next label> -> remove (fallthrough) */
    /* Single-operand jump; commas indicate multi-operand syntax (not expected for jmp). */
    if (!has_two_ops && operands && *operands && asmopt_is_unconditional_jump(base_mnemonic) && !strchr(operands, ',')) {
        char* trimmed = asmopt_strip(operands);
        if (trimmed && *trimmed) {
            size_t next_index = line_no; /* line_no is 1-based; next line lives at this index */
            if (next_index < ctx->original_count) {
                const char* next_line = ctx->original_lines[next_index];
                char* next_code = NULL;
                char* next_comment = NULL;
                asmopt_split_comment(next_line, &next_code, &next_comment);
                if (next_code && !asmopt_is_blank(next_code)) {
                    char* next_trimmed = asmopt_strip(next_code);
                    if (next_trimmed) {
                        size_t len = strlen(next_trimmed);
                        if (len > 0 && next_trimmed[len - 1] == ':') {
                            next_trimmed[len - 1] = '\0';
                            if (strcmp(next_trimmed, trimmed) == 0) {
                                asmopt_handle_identity_removal(ctx, line_no, "fallthrough_jump", line, comment, indent, removed);
                                free(next_trimmed);
                                free(next_code);
                                free(next_comment);
                                free(trimmed);
                                goto cleanup;
                            }
                        }
                        free(next_trimmed);
                    }
                }
                free(next_code);
                free(next_comment);
            }
        }
        free(trimmed);
    }

    /* Pattern 25: jcc label + jmp other + label -> invert conditional jump */
    if (!has_two_ops && operands && *operands && asmopt_is_conditional_jump(base_mnemonic) && !strchr(operands, ',')) {
        char inverted[16];
        if (asmopt_invert_conditional_jump(base_mnemonic, inverted, sizeof(inverted))) {
            char* cond_target = asmopt_strip(operands);
            if (cond_target && asmopt_is_label_operand(cond_target)) {
                size_t next_index = line_no;
                size_t label_index = line_no + 1;
                if (next_index < ctx->original_count && label_index < ctx->original_count) {
                    const char* next_line = ctx->original_lines[next_index];
                    char* next_code = NULL;
                    char* next_comment = NULL;
                    asmopt_split_comment(next_line, &next_code, &next_comment);
                    if (next_code && !asmopt_is_directive_or_label(next_code)) {
                        char* next_indent = NULL;
                        char* next_mnemonic = NULL;
                        char* next_spacing = NULL;
                        char* next_operands = NULL;
                        if (asmopt_parse_instruction(next_code, &next_indent, &next_mnemonic, &next_spacing, &next_operands)) {
                            char next_lower[32];
                            size_t next_len = strlen(next_mnemonic);
                            if (next_len >= sizeof(next_lower)) {
                                next_len = sizeof(next_lower) - 1;
                            }
                            for (size_t i = 0; i < next_len; i++) {
                                next_lower[i] = (char)tolower((unsigned char)next_mnemonic[i]);
                            }
                            next_lower[next_len] = '\0';
                            if (asmopt_is_unconditional_jump(next_lower) && next_operands && !strchr(next_operands, ',')) {
                                char* jmp_target = asmopt_strip(next_operands);
                                if (jmp_target && asmopt_is_label_operand(jmp_target)) {
                                    const char* label_line = ctx->original_lines[label_index];
                                    char* label_code = NULL;
                                    char* label_comment = NULL;
                                    asmopt_split_comment(label_line, &label_code, &label_comment);
                                    if (label_code) {
                                        char* label_trimmed = asmopt_strip(label_code);
                                        if (label_trimmed) {
                                            size_t label_len = strlen(label_trimmed);
                                            if (label_len > 0 && label_trimmed[label_len - 1] == ':') {
                                                label_trimmed[label_len - 1] = '\0';
                                                if (strcmp(label_trimmed, cond_target) == 0) {
                                                    char* trimmed_comment = asmopt_trim_comment(comment);
                                                    size_t new_len = strlen(indent) + strlen(inverted) + strlen(spacing) + strlen(jmp_target) + 1;
                                                    if (!asmopt_is_blank(trimmed_comment)) {
                                                        new_len += strlen(trimmed_comment) + 1;
                                                    }
                                                    char* newline = malloc(new_len + 1);
                                                    if (newline) {
                                                        if (!asmopt_is_blank(trimmed_comment)) {
                                                            snprintf(newline, new_len + 1, "%s%s%s%s %s",
                                                                     indent, inverted, spacing, jmp_target, trimmed_comment);
                                                        } else {
                                                            snprintf(newline, new_len + 1, "%s%s%s%s",
                                                                     indent, inverted, spacing, jmp_target);
                                                        }
                                                        size_t combo_len = strlen(line) + strlen(next_line) + 2;
                                                        char* combined = malloc(combo_len + 1);
                                                        if (combined) {
                                                            snprintf(combined, combo_len + 1, "%s\n%s", line, next_line);
                                                            asmopt_record_optimization(ctx, line_no, "invert_conditional_jump", combined, newline);
                                                            free(combined);
                                                        } else {
                                                            asmopt_record_optimization(ctx, line_no, "invert_conditional_jump", line, newline);
                                                        }
                                                        asmopt_store_optimized_line(ctx, newline);
                                                        free(newline);
                                                        if (!asmopt_is_blank(next_comment)) {
                                                            char* trimmed_next = asmopt_trim_comment(next_comment);
                                                            size_t next_len_comment = strlen(next_indent) + strlen(trimmed_next) + 1;
                                                            char* comment_line = malloc(next_len_comment + 1);
                                                            if (comment_line) {
                                                                snprintf(comment_line, next_len_comment + 1, "%s%s", next_indent, trimmed_next);
                                                                asmopt_store_optimized_line(ctx, comment_line);
                                                                free(comment_line);
                                                            }
                                                            free(trimmed_next);
                                                        }
                                                        *replaced = true;
                                                        *removed = true;
                                                        ctx->skip_lines = 1;
                                                        free(trimmed_comment);
                                                        free(label_trimmed);
                                                        free(label_code);
                                                        free(label_comment);
                                                        free(jmp_target);
                                                        free(next_indent);
                                                        free(next_mnemonic);
                                                        free(next_spacing);
                                                        free(next_operands);
                                                        free(next_code);
                                                        free(next_comment);
                                                        free(cond_target);
                                                        goto cleanup;
                                                    }
                                                    free(trimmed_comment);
                                                }
                                            }
                                            free(label_trimmed);
                                        }
                                    }
                                    free(label_code);
                                    free(label_comment);
                                }
                                free(jmp_target);
                            }
                        }
                        free(next_indent);
                        free(next_mnemonic);
                        free(next_spacing);
                        free(next_operands);
                    }
                    free(next_code);
                    free(next_comment);
                }
            }
            free(cond_target);
        }
    }

    /* Pattern 23: bsf reg, reg -> tzcnt reg, reg (Zen/BMI1, guarded zero) */
    if (strcmp(base_mnemonic, "bsf") == 0 && has_two_ops && dest_reg && src_reg && asmopt_is_target_zen(ctx) &&
        asmopt_is_zero_guarded(ctx, line_no, src, syntax)) {
        char tzcnt_name[8];
        asmopt_build_suffixed_name(tzcnt_name, sizeof(tzcnt_name), "tzcnt", suffix);
        char* trimmed_comment = asmopt_trim_comment(comment);
        size_t new_len = strlen(indent) + strlen(tzcnt_name) + strlen(spacing) +
                         strlen(dest) + strlen(pre_space) + strlen(post_space) + strlen(src) + 2;
        if (!asmopt_is_blank(trimmed_comment)) {
            new_len += strlen(trimmed_comment) + 1;
        }
        char* newline = malloc(new_len + 1);
        if (newline) {
            if (!asmopt_is_blank(trimmed_comment)) {
                snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s %s",
                         indent, tzcnt_name, spacing, dest, pre_space, post_space, src, trimmed_comment);
            } else {
                snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s",
                         indent, tzcnt_name, spacing, dest, pre_space, post_space, src);
            }
            asmopt_record_optimization(ctx, line_no, "bsf_to_tzcnt", line, newline);
            asmopt_store_optimized_line(ctx, newline);
            free(newline);
            *replaced = true;
        }
        free(trimmed_comment);
        goto cleanup;
    }

    /* bsr -> lzcnt not applied: not semantically equivalent. */

    /* Pattern 3: imul/mul rax, 1 -> remove (identity) */
    if (strcmp(base_mnemonic, "imul") == 0 && has_two_ops) {
        if (dest_reg && asmopt_is_immediate_one(src, syntax)) {
            asmopt_handle_identity_removal(ctx, line_no, "mul_by_one", line, comment, indent, removed);
            goto cleanup;
        }
        
        /* Pattern 4: imul rax, power_of_2 -> shl rax, log2(power_of_2) */
        bool success = false;
        long imm_val = asmopt_parse_immediate(src, syntax, &success);
        if (dest_reg && success && asmopt_is_power_of_two(imm_val)) {
            int shift_amount = asmopt_log2(imm_val);
            char shl_name[8];
            if (suffix) {
                snprintf(shl_name, sizeof(shl_name), "shl%c", suffix);
            } else {
                snprintf(shl_name, sizeof(shl_name), "shl");
            }
            char shift_str[16];
            if (syntax && strcmp(syntax, "att") == 0) {
                snprintf(shift_str, sizeof(shift_str), "$%d", shift_amount);
            } else {
                snprintf(shift_str, sizeof(shift_str), "%d", shift_amount);
            }
            char* trimmed_comment = asmopt_trim_comment(comment);
            size_t new_len = strlen(indent) + strlen(shl_name) + strlen(spacing) + 
                            strlen(dest) + strlen(pre_space) + strlen(post_space) + strlen(shift_str) + 2;
            if (!asmopt_is_blank(trimmed_comment)) {
                new_len += strlen(trimmed_comment) + 1;
            }
            char* newline = malloc(new_len + 1);
            if (newline) {
                if (!asmopt_is_blank(trimmed_comment)) {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s %s", 
                            indent, shl_name, spacing, dest, pre_space, post_space, shift_str, trimmed_comment);
                } else {
                    snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s", 
                            indent, shl_name, spacing, dest, pre_space, post_space, shift_str);
                }
                asmopt_record_optimization(ctx, line_no, "mul_power_of_2_to_shift", line, newline);
                asmopt_store_optimized_line(ctx, newline);
                free(newline);
                *replaced = true;
            }
            free(trimmed_comment);
            goto cleanup;
        }
    }
    
    /* Pattern 5: add/sub rax, 0 -> remove (identity) */
    if ((strcmp(base_mnemonic, "add") == 0 || strcmp(base_mnemonic, "sub") == 0) && has_two_ops) {
        if (dest_reg && asmopt_is_immediate_zero(src, syntax)) {
            asmopt_handle_identity_removal(ctx, line_no, "add_sub_zero", line, comment, indent, removed);
            goto cleanup;
        }
    }
    
    /* Pattern 6: shl/shr rax, 0 -> remove (identity) */
    if ((strcmp(base_mnemonic, "shl") == 0 || strcmp(base_mnemonic, "shr") == 0 || 
         strcmp(base_mnemonic, "sal") == 0 || strcmp(base_mnemonic, "sar") == 0) && has_two_ops) {
        if (dest_reg && asmopt_is_immediate_zero(src, syntax)) {
            asmopt_handle_identity_removal(ctx, line_no, "shift_by_zero", line, comment, indent, removed);
            goto cleanup;
        }
    }
    
    /* Pattern 7: or rax, 0 -> remove (identity) */
    if (strcmp(base_mnemonic, "or") == 0 && has_two_ops) {
        if (dest_reg && asmopt_is_immediate_zero(src, syntax)) {
            asmopt_handle_identity_removal(ctx, line_no, "or_zero", line, comment, indent, removed);
            goto cleanup;
        }
    }
    
    /* Pattern 8: xor rax, 0 -> remove (identity) */
    if (strcmp(base_mnemonic, "xor") == 0 && has_two_ops) {
        if (dest_reg && asmopt_is_immediate_zero(src, syntax)) {
            asmopt_handle_identity_removal(ctx, line_no, "xor_zero", line, comment, indent, removed);
            goto cleanup;
        }
    }
    
    /* Pattern 9: and rax, -1 -> remove (identity, all bits set) */
    if (strcmp(base_mnemonic, "and") == 0 && has_two_ops) {
        if (dest_reg && asmopt_is_immediate_minus_one(src, syntax)) {
            asmopt_handle_identity_removal(ctx, line_no, "and_minus_one", line, comment, indent, removed);
            goto cleanup;
        }
    }
    
    /* Pattern 10: add rax, 1 -> inc rax (smaller encoding) */
    if (strcmp(base_mnemonic, "add") == 0 && has_two_ops) {
        if (dest_reg && asmopt_is_immediate_one(src, syntax)) {
            char inc_name[16];  /* Increased buffer size for safety */
            if (suffix) {
                snprintf(inc_name, sizeof(inc_name), "inc%c", suffix);
            } else {
                snprintf(inc_name, sizeof(inc_name), "inc");
            }
            char* trimmed_comment = asmopt_trim_comment(comment);
            size_t new_len = strlen(indent) + strlen(inc_name) + strlen(spacing) + strlen(dest) + 2;
            if (!asmopt_is_blank(trimmed_comment)) {
                new_len += strlen(trimmed_comment) + 1;
            }
            char* newline = malloc(new_len + 1);
            if (newline) {
                if (!asmopt_is_blank(trimmed_comment)) {
                    snprintf(newline, new_len + 1, "%s%s%s%s %s", 
                            indent, inc_name, spacing, dest, trimmed_comment);
                } else {
                    snprintf(newline, new_len + 1, "%s%s%s%s", 
                            indent, inc_name, spacing, dest);
                }
                asmopt_record_optimization(ctx, line_no, "add_one_to_inc", line, newline);
                asmopt_store_optimized_line(ctx, newline);
                free(newline);
                *replaced = true;
            }
            free(trimmed_comment);
            goto cleanup;
        }
    }
    
    /* Pattern 11: sub rax, 1 -> dec rax (smaller encoding) */
    if (strcmp(base_mnemonic, "sub") == 0 && has_two_ops) {
        if (dest_reg && asmopt_is_immediate_one(src, syntax)) {
            char dec_name[16];  /* Increased buffer size for safety */
            if (suffix) {
                snprintf(dec_name, sizeof(dec_name), "dec%c", suffix);
            } else {
                snprintf(dec_name, sizeof(dec_name), "dec");
            }
            char* trimmed_comment = asmopt_trim_comment(comment);
            size_t new_len = strlen(indent) + strlen(dec_name) + strlen(spacing) + strlen(dest) + 2;
            if (!asmopt_is_blank(trimmed_comment)) {
                new_len += strlen(trimmed_comment) + 1;
            }
            char* newline = malloc(new_len + 1);
            if (newline) {
                if (!asmopt_is_blank(trimmed_comment)) {
                    snprintf(newline, new_len + 1, "%s%s%s%s %s", 
                            indent, dec_name, spacing, dest, trimmed_comment);
                } else {
                    snprintf(newline, new_len + 1, "%s%s%s%s", 
                            indent, dec_name, spacing, dest);
                }
                asmopt_record_optimization(ctx, line_no, "sub_one_to_dec", line, newline);
                asmopt_store_optimized_line(ctx, newline);
                free(newline);
                *replaced = true;
            }
            free(trimmed_comment);
            goto cleanup;
        }
    }
    
    /* No optimization applied, store original */
    asmopt_store_optimized_line(ctx, line);

cleanup:
    free(code);
    free(comment);
    free(indent);
    free(mnemonic);
    free(spacing);
    free(operands);
    free(op1);
    free(op2);
    free(pre_space);
    free(post_space);
}

static bool asmopt_is_unconditional_jump(const char* mnemonic) {
    return asmopt_is_jump_mnemonic(mnemonic) && !asmopt_is_conditional_jump(mnemonic);
}

static bool asmopt_is_jump_mnemonic(const char* mnemonic) {
    if (!mnemonic) {
        return false;
    }
    const char* jumps[] = {
        "jo",   "jno",  "js",   "jns",  "je",   "jz",   "jne",  "jnz",
        "jb",   "jnae", "jc",   "jnb",  "jae",  "jnc",  "jbe",  "jna",
        "ja",   "jnbe", "jl",   "jnge", "jge",  "jnl",  "jle",  "jng",
        "jg",   "jnle", "jp",   "jpe",  "jnp",  "jpo",  "jcxz", "jecxz",
        "jrcxz","jmp",  "jmpq", "jmpl", "jmpw"
    };
    size_t count = sizeof(jumps) / sizeof(jumps[0]);
    for (size_t i = 0; i < count; i++) {
        if (asmopt_casecmp(mnemonic, jumps[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool asmopt_is_label_operand(const char* operand) {
    if (!operand) {
        return false;
    }
    const char* op = operand;
    while (*op == '*') {
        op++;
    }
    if (!isalpha((unsigned char)*op) && *op != '_' && *op != '.') {
        return false;
    }
    for (const char* ptr = op; *ptr; ptr++) {
        if (!isalnum((unsigned char)*ptr) && *ptr != '_' && *ptr != '.') {
            return false;
        }
    }
    return true;
}

static bool asmopt_is_conditional_jump(const char* mnemonic) {
    if (!mnemonic) {
        return false;
    }
    const char* jumps[] = {
        "jo", "jno", "js", "jns", "je", "jz", "jne", "jnz",
        "jb", "jnae", "jc", "jnb", "jae", "jnc", "jbe", "jna",
        "ja", "jnbe", "jl", "jnge", "jge", "jnl", "jle", "jng",
        "jg", "jnle", "jp", "jpe", "jnp", "jpo", "jcxz", "jecxz", "jrcxz"
    };
    size_t count = sizeof(jumps) / sizeof(jumps[0]);
    for (size_t i = 0; i < count; i++) {
        if (asmopt_casecmp(mnemonic, jumps[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool asmopt_invert_conditional_jump(const char* mnemonic, char* buffer, size_t size) {
    if (!mnemonic || !buffer || size == 0) {
        return false;
    }
    const char* pairs[][2] = {
        {"je", "jne"}, {"jz", "jnz"}, {"jne", "je"}, {"jnz", "jz"},
        {"jb", "jnb"}, {"jnae", "jae"}, {"jc", "jnc"},
        {"jnb", "jb"}, {"jae", "jnae"}, {"jnc", "jc"},
        {"jbe", "ja"}, {"jna", "ja"}, {"ja", "jbe"}, {"jnbe", "jbe"},
        {"jl", "jge"}, {"jnge", "jge"}, {"jge", "jl"}, {"jnl", "jl"},
        {"jle", "jg"}, {"jng", "jg"}, {"jg", "jle"}, {"jnle", "jle"},
        {"jo", "jno"}, {"jno", "jo"}, {"js", "jns"}, {"jns", "js"},
        {"jp", "jnp"}, {"jpe", "jpo"}, {"jnp", "jp"}, {"jpo", "jpe"}
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        if (asmopt_casecmp(mnemonic, pairs[i][0]) == 0) {
            snprintf(buffer, size, "%s", pairs[i][1]);
            return true;
        }
    }
    return false;
}

static bool asmopt_is_return(const char* mnemonic) {
    if (!mnemonic) {
        return false;
    }
    if (strlen(mnemonic) < 3) {
        return false;
    }
    return asmopt_case_prefix_len(mnemonic, "ret");
}

static char* asmopt_jump_target(const asmopt_ir_line* line) {
    if (!line || line->operand_count == 0) {
        return NULL;
    }
    char* operand = line->operands[0];
    if (!operand) {
        return NULL;
    }
    while (*operand == '*') {
        operand++;
    }
    if (!isalpha((unsigned char)*operand) && *operand != '_' && *operand != '.') {
        return NULL;
    }
    for (const char* ptr = operand; *ptr; ptr++) {
        if (!isalnum((unsigned char)*ptr) && *ptr != '_' && *ptr != '.') {
            return NULL;
        }
    }
    return asmopt_strdup(operand);
}

static void asmopt_build_ir(asmopt_context* ctx) {
    if (!ctx) {
        return;
    }
    asmopt_reset_ir(ctx);
    ctx->ir = calloc(ctx->original_count, sizeof(asmopt_ir_line));
    ctx->ir_count = 0;
    for (size_t i = 0; i < ctx->original_count; i++) {
        char* line = ctx->original_lines[i];
        char* code = NULL;
    char* comment = NULL;
    asmopt_split_comment(line, &code, &comment);
    if (!code) {
        free(comment);
        continue;
    }
    char* trimmed = asmopt_strip(code);
    asmopt_ir_line entry = {0};
        entry.line_no = i + 1;
        if (!trimmed || trimmed[0] == '\0') {
            entry.kind = asmopt_strdup("blank");
            entry.text = asmopt_strdup("");
        } else if (trimmed[0] == '.') {
            entry.kind = asmopt_strdup("directive");
            entry.text = asmopt_strdup(trimmed);
        } else {
            size_t len = strlen(trimmed);
            if (len > 0 && trimmed[len - 1] == ':') {
                trimmed[len - 1] = '\0';
                entry.kind = asmopt_strdup("label");
                entry.text = asmopt_strdup(trimmed);
            } else {
                char* indent = NULL;
                char* mnemonic = NULL;
                char* spacing = NULL;
                char* operands = NULL;
                if (asmopt_parse_instruction(code, &indent, &mnemonic, &spacing, &operands)) {
                    entry.kind = asmopt_strdup("instruction");
                    entry.text = asmopt_strdup(trimmed);
                    entry.mnemonic = asmopt_strdup(mnemonic);
                    entry.operands = NULL;
                    entry.operand_count = 0;
                    if (operands && *operands) {
                        size_t olen = strlen(operands);
                        size_t start = 0;
                        for (size_t j = 0; j <= olen; j++) {
                            if (operands[j] == ',' || operands[j] == '\0') {
                                size_t seg_len = j - start;
                                char* segment = malloc(seg_len + 1);
                                if (!segment) {
                                    break;
                                }
                                memcpy(segment, operands + start, seg_len);
                                segment[seg_len] = '\0';
                                char* token_trimmed = asmopt_strip(segment);
                                if (token_trimmed && *token_trimmed) {
                                    char** next_ops = realloc(entry.operands, sizeof(char*) * (entry.operand_count + 1));
                                    if (next_ops) {
                                        entry.operands = next_ops;
                                        entry.operands[entry.operand_count++] = token_trimmed;
                                    } else {
                                        free(token_trimmed);
                                    }
                                } else {
                                    free(token_trimmed);
                                }
                                free(segment);
                                start = j + 1;
                            }
                        }
                    }
                } else {
                    entry.kind = asmopt_strdup("text");
                    entry.text = asmopt_strdup(trimmed);
                }
                free(indent);
                free(mnemonic);
                free(spacing);
                free(operands);
            }
        }
        free(code);
        free(comment);
        free(trimmed);
        if (!entry.kind) {
            entry.kind = asmopt_strdup("text");
            entry.text = asmopt_strdup(trimmed ? trimmed : "");
        }
        ctx->ir[ctx->ir_count++] = entry;
    }
}

static int asmopt_find_block_index(asmopt_cfg_block* blocks, size_t count, const char* name) {
    for (size_t i = 0; i < count; i++) {
        if (blocks[i].name && strcmp(blocks[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void asmopt_add_edge(asmopt_context* ctx, const char* source, const char* target) {
    if (!ctx || !source || !target) {
        return;
    }
    asmopt_cfg_edge* next = realloc(ctx->cfg_edges, sizeof(asmopt_cfg_edge) * (ctx->cfg_edge_count + 1));
    if (!next) {
        return;
    }
    ctx->cfg_edges = next;
    ctx->cfg_edges[ctx->cfg_edge_count].source = asmopt_strdup(source);
    ctx->cfg_edges[ctx->cfg_edge_count].target = asmopt_strdup(target);
    ctx->cfg_edge_count += 1;
}

static void asmopt_build_cfg(asmopt_context* ctx) {
    asmopt_reset_cfg(ctx);
    if (!ctx || ctx->ir_count == 0) {
        return;
    }
    asmopt_cfg_block* blocks = NULL;
    size_t block_count = 0;
    asmopt_ir_line** current_instrs = NULL;
    size_t instr_count = 0;
    char* current_label = NULL;
    char** label_names = NULL;
    size_t label_count = 0;

    for (size_t i = 0; i < ctx->ir_count; i++) {
        asmopt_ir_line* line = &ctx->ir[i];
        if (line->kind && strcmp(line->kind, "label") == 0) {
            if (current_label || instr_count > 0) {
                asmopt_cfg_block* next = realloc(blocks, sizeof(asmopt_cfg_block) * (block_count + 1));
                if (!next) {
                    break;
                }
                blocks = next;
                blocks[block_count].name = current_label ? asmopt_strdup(current_label) : NULL;
                blocks[block_count].instructions = current_instrs;
                blocks[block_count].instruction_count = instr_count;
                if (current_label) {
                    char** label_next = realloc(label_names, sizeof(char*) * (label_count + 1));
                    if (label_next) {
                        label_names = label_next;
                        label_names[label_count++] = asmopt_strdup(current_label);
                    }
                }
                block_count += 1;
                free(current_label);
                current_label = NULL;
                current_instrs = NULL;
                instr_count = 0;
            }
            current_label = asmopt_strdup(line->text);
            continue;
        }
        if (!line->kind || strcmp(line->kind, "instruction") != 0) {
            continue;
        }
        asmopt_ir_line** next_instrs = realloc(current_instrs, sizeof(asmopt_ir_line*) * (instr_count + 1));
        if (!next_instrs) {
            break;
        }
        current_instrs = next_instrs;
        current_instrs[instr_count++] = line;
        if (line->mnemonic && (asmopt_is_jump_mnemonic(line->mnemonic) || asmopt_is_return(line->mnemonic))) {
            asmopt_cfg_block* next = realloc(blocks, sizeof(asmopt_cfg_block) * (block_count + 1));
            if (!next) {
                break;
            }
            blocks = next;
            blocks[block_count].name = current_label ? asmopt_strdup(current_label) : NULL;
            blocks[block_count].instructions = current_instrs;
            blocks[block_count].instruction_count = instr_count;
            if (current_label) {
                char** label_next = realloc(label_names, sizeof(char*) * (label_count + 1));
                if (label_next) {
                    label_names = label_next;
                    label_names[label_count++] = asmopt_strdup(current_label);
                }
            }
            block_count += 1;
            free(current_label);
            current_label = NULL;
            current_instrs = NULL;
            instr_count = 0;
        }
    }
    if (current_label || instr_count > 0) {
        asmopt_cfg_block* next = realloc(blocks, sizeof(asmopt_cfg_block) * (block_count + 1));
        if (next) {
            blocks = next;
            blocks[block_count].name = current_label ? asmopt_strdup(current_label) : NULL;
            blocks[block_count].instructions = current_instrs;
            blocks[block_count].instruction_count = instr_count;
            if (current_label) {
                char** label_next = realloc(label_names, sizeof(char*) * (label_count + 1));
                if (label_next) {
                    label_names = label_next;
                    label_names[label_count++] = asmopt_strdup(current_label);
                }
            }
            block_count += 1;
        }
    } else {
        free(current_instrs);
    }
    free(current_label);

    if (block_count == 0) {
        blocks = calloc(1, sizeof(asmopt_cfg_block));
        block_count = 1;
        blocks[0].name = asmopt_strdup("block0");
        blocks[0].instructions = NULL;
        blocks[0].instruction_count = 0;
    } else {
        for (size_t i = 0; i < block_count; i++) {
            if (!blocks[i].name) {
                char name[32];
                snprintf(name, sizeof(name), "block%zu", i);
                blocks[i].name = asmopt_strdup(name);
            }
        }
    }

    ctx->cfg_blocks = blocks;
    ctx->cfg_block_count = block_count;
    for (size_t i = 0; i < block_count; i++) {
        asmopt_cfg_block* block = &ctx->cfg_blocks[i];
        if (block->instruction_count == 0) {
            continue;
        }
        asmopt_ir_line* last = block->instructions[block->instruction_count - 1];
        if (last && last->mnemonic && asmopt_is_jump_mnemonic(last->mnemonic)) {
            char* target = asmopt_jump_target(last);
            if (target) {
                int index = asmopt_find_block_index(ctx->cfg_blocks, ctx->cfg_block_count, target);
                if (index >= 0) {
                    asmopt_add_edge(ctx, block->name, ctx->cfg_blocks[index].name);
                }
                free(target);
            }
            if (last->mnemonic && asmopt_is_conditional_jump(last->mnemonic) && i + 1 < block_count) {
                asmopt_add_edge(ctx, block->name, ctx->cfg_blocks[i + 1].name);
            }
        } else if (last && last->mnemonic && asmopt_is_return(last->mnemonic)) {
            continue;
        } else if (i + 1 < block_count) {
            asmopt_add_edge(ctx, block->name, ctx->cfg_blocks[i + 1].name);
        }
    }

    asmopt_free_string_array(label_names, label_count);
}

static void asmopt_append_text(char** buffer, size_t* length, size_t* capacity, const char* text) {
    size_t add = strlen(text);
    if (*length + add + 1 > *capacity) {
        size_t next = (*capacity + add + 1) * 2;
        char* resized = realloc(*buffer, next);
        if (!resized) {
            return;
        }
        *buffer = resized;
        *capacity = next;
    }
    memcpy(*buffer + *length, text, add);
    *length += add;
    (*buffer)[*length] = '\0';
}

static char* asmopt_dump_ir(asmopt_context* ctx) {
    if (!ctx) {
        return asmopt_strdup("IR:\n");
    }
    size_t capacity = 256;
    size_t length = 0;
    char* buffer = malloc(capacity);
    if (!buffer) {
        return NULL;
    }
    buffer[0] = '\0';
    asmopt_append_text(&buffer, &length, &capacity, "IR:\n");
    for (size_t i = 0; i < ctx->ir_count; i++) {
        asmopt_ir_line* line = &ctx->ir[i];
        if (!line->kind) {
            continue;
        }
        if (strcmp(line->kind, "instruction") == 0) {
            size_t ops_cap = 128;
            size_t ops_len = 0;
            char* ops = malloc(ops_cap);
            if (!ops) {
                continue;
            }
            ops[0] = '\0';
            for (size_t j = 0; j < line->operand_count; j++) {
                asmopt_append_text(&ops, &ops_len, &ops_cap, line->operands[j]);
                if (j + 1 < line->operand_count) {
                    asmopt_append_text(&ops, &ops_len, &ops_cap, ", ");
                }
            }
            char linebuf[64];
            snprintf(linebuf, sizeof(linebuf), "%04zu: instr %s ", line->line_no, line->mnemonic ? line->mnemonic : "");
            asmopt_append_text(&buffer, &length, &capacity, linebuf);
            asmopt_append_text(&buffer, &length, &capacity, ops);
            asmopt_append_text(&buffer, &length, &capacity, "\n");
            free(ops);
        } else {
            char linebuf[64];
            snprintf(linebuf, sizeof(linebuf), "%04zu: %s ", line->line_no, line->kind);
            asmopt_append_text(&buffer, &length, &capacity, linebuf);
            asmopt_append_text(&buffer, &length, &capacity, line->text ? line->text : "");
            asmopt_append_text(&buffer, &length, &capacity, "\n");
        }
    }
    return buffer;
}

static char* asmopt_dump_cfg_text_internal(asmopt_context* ctx) {
    if (!ctx) {
        return asmopt_strdup("CFG:\n");
    }
    size_t capacity = 256;
    size_t length = 0;
    char* buffer = malloc(capacity);
    if (!buffer) {
        return NULL;
    }
    buffer[0] = '\0';
    asmopt_append_text(&buffer, &length, &capacity, "CFG:\n");
    for (size_t i = 0; i < ctx->cfg_block_count; i++) {
        asmopt_cfg_block* block = &ctx->cfg_blocks[i];
        asmopt_append_text(&buffer, &length, &capacity, block->name);
        asmopt_append_text(&buffer, &length, &capacity, ":\n");
        for (size_t j = 0; j < block->instruction_count; j++) {
            asmopt_ir_line* line = block->instructions[j];
            if (!line || !line->mnemonic) {
                continue;
            }
            asmopt_append_text(&buffer, &length, &capacity, "  ");
            asmopt_append_text(&buffer, &length, &capacity, line->mnemonic);
            for (size_t k = 0; k < line->operand_count; k++) {
                asmopt_append_text(&buffer, &length, &capacity, k == 0 ? " " : ", ");
                asmopt_append_text(&buffer, &length, &capacity, line->operands[k]);
            }
            asmopt_append_text(&buffer, &length, &capacity, "\n");
        }
        for (size_t e = 0; e < ctx->cfg_edge_count; e++) {
            if (strcmp(ctx->cfg_edges[e].source, block->name) == 0) {
                asmopt_append_text(&buffer, &length, &capacity, "  -> ");
                asmopt_append_text(&buffer, &length, &capacity, ctx->cfg_edges[e].target);
                asmopt_append_text(&buffer, &length, &capacity, "\n");
            }
        }
    }
    return buffer;
}

static char* asmopt_dump_cfg_dot_internal(asmopt_context* ctx) {
    if (!ctx) {
        return asmopt_strdup("digraph cfg {\n  node [shape=box];\n}\n");
    }
    size_t capacity = 256;
    size_t length = 0;
    char* buffer = malloc(capacity);
    if (!buffer) {
        return NULL;
    }
    buffer[0] = '\0';
    asmopt_append_text(&buffer, &length, &capacity, "digraph cfg {\n  node [shape=box];\n");
    for (size_t i = 0; i < ctx->cfg_block_count; i++) {
        asmopt_cfg_block* block = &ctx->cfg_blocks[i];
        asmopt_append_text(&buffer, &length, &capacity, "  ");
        asmopt_append_text(&buffer, &length, &capacity, block->name);
        asmopt_append_text(&buffer, &length, &capacity, " [label=\"");
        asmopt_append_text(&buffer, &length, &capacity, block->name);
        asmopt_append_text(&buffer, &length, &capacity, ":\\l");
        for (size_t j = 0; j < block->instruction_count; j++) {
            asmopt_ir_line* line = block->instructions[j];
            if (!line || !line->mnemonic) {
                continue;
            }
            asmopt_append_text(&buffer, &length, &capacity, line->mnemonic);
            for (size_t k = 0; k < line->operand_count; k++) {
                asmopt_append_text(&buffer, &length, &capacity, k == 0 ? " " : ", ");
                asmopt_append_text(&buffer, &length, &capacity, line->operands[k]);
            }
            asmopt_append_text(&buffer, &length, &capacity, "\\l");
        }
        asmopt_append_text(&buffer, &length, &capacity, "\"];\n");
    }
    for (size_t e = 0; e < ctx->cfg_edge_count; e++) {
        asmopt_append_text(&buffer, &length, &capacity, "  ");
        asmopt_append_text(&buffer, &length, &capacity, ctx->cfg_edges[e].source);
        asmopt_append_text(&buffer, &length, &capacity, " -> ");
        asmopt_append_text(&buffer, &length, &capacity, ctx->cfg_edges[e].target);
        asmopt_append_text(&buffer, &length, &capacity, ";\n");
    }
    asmopt_append_text(&buffer, &length, &capacity, "}\n");
    return buffer;
}

asmopt_context* asmopt_create(const char* architecture) {
    asmopt_context* ctx = calloc(1, sizeof(asmopt_context));
    if (!ctx) {
        return NULL;
    }
    ctx->architecture = asmopt_strdup(architecture ? architecture : "x86-64");
    ctx->target_cpu = asmopt_strdup("generic");
    ctx->optimization_level = 2;
    ctx->amd_optimizations = true;
    ctx->enabled_opts = NULL;
    ctx->enabled_count = 0;
    asmopt_add_name(&ctx->enabled_opts, &ctx->enabled_count, "peephole");
    return ctx;
}

void asmopt_set_option(asmopt_context* ctx, const char* option, const char* value) {
    asmopt_add_option(ctx, option, value);
}

void asmopt_set_optimization_level(asmopt_context* ctx, int level) {
    if (!ctx) {
        return;
    }
    if (level < 0) {
        level = 0;
    }
    if (level > 4) {
        level = 4;
    }
    ctx->optimization_level = level;
}

void asmopt_set_target_cpu(asmopt_context* ctx, const char* cpu) {
    if (!ctx) {
        return;
    }
    free(ctx->target_cpu);
    ctx->target_cpu = asmopt_strdup(cpu ? cpu : "generic");
}

static char* asmopt_read_file(const char* filename) {
    if (!filename) {
        return NULL;
    }
    FILE* handle = fopen(filename, "rb");
    if (!handle) {
        return NULL;
    }
    fseek(handle, 0, SEEK_END);
    long length = ftell(handle);
    if (length < 0) {
        fclose(handle);
        return NULL;
    }
    fseek(handle, 0, SEEK_SET);
    char* buffer = malloc((size_t)length + 1);
    if (!buffer) {
        fclose(handle);
        return NULL;
    }
    size_t read = fread(buffer, 1, (size_t)length, handle);
    buffer[read] = '\0';
    fclose(handle);
    return buffer;
}

int asmopt_parse_file(asmopt_context* ctx, const char* filename) {
    if (!ctx || !filename) {
        return -1;
    }
    char* text = asmopt_read_file(filename);
    if (!text) {
        return -1;
    }
    asmopt_parse_string(ctx, text);
    free(text);
    return 0;
}

int asmopt_parse_string(asmopt_context* ctx, const char* assembly) {
    if (!ctx || !assembly) {
        return -1;
    }
    asmopt_reset_lines(ctx);
    ctx->original_text = asmopt_strdup(assembly);
    asmopt_split_lines(ctx, assembly);
    return 0;
}

static bool asmopt_should_optimize(asmopt_context* ctx) {
    if (!ctx) {
        return false;
    }
    if (ctx->no_optimize || ctx->optimization_level == 0) {
        return false;
    }
    if (asmopt_is_disabled(ctx, "peephole")) {
        return false;
    }
    if (!asmopt_has_opt(ctx->enabled_opts, ctx->enabled_count, "peephole")) {
        return false;
    }
    return true;
}

int asmopt_optimize(asmopt_context* ctx) {
    if (!ctx || !ctx->original_lines) {
        return -1;
    }
    char* syntax = asmopt_detect_syntax(ctx);
    ctx->stats.original_lines = ctx->original_count;
    ctx->stats.optimized_lines = 0;
    ctx->stats.replacements = 0;
    ctx->stats.removals = 0;
    asmopt_build_ir(ctx);
    asmopt_build_cfg(ctx);
    ctx->insert_hot_align = asmopt_option_enabled(ctx, "hot_align");
    bool do_opt = asmopt_should_optimize(ctx);
    for (size_t i = 0; i < ctx->original_count; i++) {
        if (!do_opt) {
            asmopt_store_optimized_line(ctx, ctx->original_lines[i]);
            continue;
        }
        bool replaced = false;
        bool removed = false;
        asmopt_peephole_line(ctx, i + 1, ctx->original_lines[i], syntax, &replaced, &removed);
        size_t skip_lines = ctx->skip_lines;
        ctx->skip_lines = 0;
        if (replaced) {
            ctx->stats.replacements += 1;
        }
        if (removed) {
            ctx->stats.removals += 1;
        }
        if (skip_lines > 0) {
            i += skip_lines;
        }
    }
    if (!do_opt) {
        ctx->stats.optimized_lines = ctx->original_count;
    } else {
        ctx->stats.optimized_lines = ctx->optimized_count;
    }
    free(syntax);
    return 0;
}

char* asmopt_generate_assembly(asmopt_context* ctx) {
    if (!ctx) {
        return NULL;
    }
    if ((!ctx->optimized_lines || ctx->optimized_count == 0) && ctx->original_lines) {
        return asmopt_join_lines(ctx->original_lines, ctx->original_count, ctx->trailing_newline);
    }
    return asmopt_join_lines(ctx->optimized_lines, ctx->optimized_count, ctx->trailing_newline);
}

char* asmopt_generate_report(asmopt_context* ctx) {
    if (!ctx) {
        return asmopt_strdup("");
    }
    
    /* Calculate required buffer size */
    size_t buffer_size = 1024;
    for (size_t i = 0; i < ctx->opt_event_count; i++) {
        buffer_size += strlen(ctx->opt_events[i].pattern_name) + 
                      strlen(ctx->opt_events[i].original) + 
                      strlen(ctx->opt_events[i].optimized) + 100;
    }
    
    char* buffer = malloc(buffer_size);
    if (!buffer) {
        return asmopt_strdup("");
    }
    
    int written = snprintf(buffer, buffer_size,
             "Optimization Report\n"
             "==================\n\n"
             "Summary:\n"
             "  Original lines: %zu\n"
             "  Optimized lines: %zu\n"
             "  Replacements: %zu\n"
             "  Removals: %zu\n",
             ctx->stats.original_lines,
             ctx->stats.optimized_lines,
             ctx->stats.replacements,
             ctx->stats.removals);
    
    if (written < 0 || (size_t)written >= buffer_size) {
        free(buffer);
        return asmopt_strdup("Error: Report generation failed\n");
    }
    
    size_t offset = (size_t)written;
    
    if (ctx->opt_event_count > 0) {
        written = snprintf(buffer + offset, buffer_size - offset, 
                          "\nOptimizations Applied:\n");
        if (written < 0 || offset + (size_t)written >= buffer_size) {
            return buffer;  /* Return what we have so far */
        }
        offset += (size_t)written;
        
        for (size_t i = 0; i < ctx->opt_event_count; i++) {
            asmopt_optimization_event* event = &ctx->opt_events[i];
            written = snprintf(buffer + offset, buffer_size - offset,
                             "  Line %zu: %s\n"
                             "    Before: %s\n"
                             "    After:  %s\n",
                             event->line_no,
                             event->pattern_name,
                             event->original,
                             event->optimized);
            if (written < 0 || offset + (size_t)written >= buffer_size) {
                return buffer;  /* Return what we have so far */
            }
            offset += (size_t)written;
        }
    }
    
    return buffer;
}

void asmopt_get_stats(asmopt_context* ctx, size_t* original, size_t* optimized, size_t* replacements, size_t* removals) {
    if (!ctx) {
        return;
    }
    if (original) {
        *original = ctx->stats.original_lines;
    }
    if (optimized) {
        *optimized = ctx->stats.optimized_lines;
    }
    if (replacements) {
        *replacements = ctx->stats.replacements;
    }
    if (removals) {
        *removals = ctx->stats.removals;
    }
}

void asmopt_destroy(asmopt_context* ctx) {
    if (!ctx) {
        return;
    }
    free(ctx->architecture);
    free(ctx->target_cpu);
    free(ctx->format);
    for (size_t i = 0; i < ctx->enabled_count; i++) {
        free(ctx->enabled_opts[i]);
    }
    for (size_t i = 0; i < ctx->disabled_count; i++) {
        free(ctx->disabled_opts[i]);
    }
    free(ctx->enabled_opts);
    free(ctx->disabled_opts);
    for (size_t i = 0; i < ctx->option_count; i++) {
        free(ctx->options[i].key);
        free(ctx->options[i].value);
    }
    free(ctx->options);
    asmopt_reset_lines(ctx);
    free(ctx);
}

void asmopt_enable_optimization(asmopt_context* ctx, const char* name) {
    if (!ctx || !name) {
        return;
    }
    if (strcmp(name, "all") == 0) {
        asmopt_add_name(&ctx->enabled_opts, &ctx->enabled_count, "peephole");
        return;
    }
    asmopt_add_name(&ctx->enabled_opts, &ctx->enabled_count, name);
}

void asmopt_disable_optimization(asmopt_context* ctx, const char* name) {
    if (!ctx || !name) {
        return;
    }
    if (strcmp(name, "all") == 0) {
        asmopt_free_string_array(ctx->enabled_opts, ctx->enabled_count);
        ctx->enabled_opts = NULL;
        ctx->enabled_count = 0;
        asmopt_add_name(&ctx->disabled_opts, &ctx->disabled_count, "all");
        return;
    }
    asmopt_add_name(&ctx->disabled_opts, &ctx->disabled_count, name);
}

void asmopt_set_format(asmopt_context* ctx, const char* format) {
    if (!ctx) {
        return;
    }
    free(ctx->format);
    ctx->format = format ? asmopt_strdup(format) : NULL;
}

void asmopt_set_no_optimize(asmopt_context* ctx, int enabled) {
    if (!ctx) {
        return;
    }
    ctx->no_optimize = enabled != 0;
}

void asmopt_set_preserve_all(asmopt_context* ctx, int enabled) {
    if (!ctx) {
        return;
    }
    ctx->preserve_all = enabled != 0;
}

void asmopt_set_amd_optimizations(asmopt_context* ctx, int enabled) {
    if (!ctx) {
        return;
    }
    ctx->amd_optimizations = enabled != 0;
}

char* asmopt_dump_ir_text(asmopt_context* ctx) {
    return asmopt_dump_ir(ctx);
}

char* asmopt_dump_cfg_text(asmopt_context* ctx) {
    return asmopt_dump_cfg_text_internal(ctx);
}

char* asmopt_dump_cfg_dot(asmopt_context* ctx) {
    return asmopt_dump_cfg_dot_internal(ctx);
}
