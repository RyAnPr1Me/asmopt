#include "asmopt.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
};

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
    memset(&ctx->stats, 0, sizeof(ctx->stats));
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
    asmopt_optimization_event* next = realloc(ctx->opt_events, 
                                              sizeof(asmopt_optimization_event) * (ctx->opt_event_count + 1));
    if (!next) {
        return;
    }
    ctx->opt_events = next;
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
    char* end = NULL;
    long value = 0;
    if (asmopt_starts_with(op, "0x")) {
        value = strtol(op, &end, 16);
    } else if (op[strlen(op) - 1] == 'h') {
        char buffer[64];
        size_t len = strlen(op) - 1;
        if (len >= sizeof(buffer)) {
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
        value = strtol(op, &end, 10);
    }
    if (end != op && *end == '\0') {
        *success = true;
    }
    return value;
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

static void asmopt_peephole_line(asmopt_context* ctx, size_t line_no, const char* line, const char* syntax, bool* replaced, bool* removed) {
    *replaced = false;
    *removed = false;
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
    const char* suffix_mnemonics[] = {"mov", "add", "sub", "xor", "and", "or", "cmp", "test", "shl", "shr", "sal", "sar"};
    bool can_have_suffix = false;
    for (size_t i = 0; i < sizeof(suffix_mnemonics) / sizeof(suffix_mnemonics[0]); i++) {
        size_t base_len = strlen(suffix_mnemonics[i]);
        if (mlen_actual == base_len + 1 && strncmp(mnemonic_lower, suffix_mnemonics[i], base_len) == 0) {
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
    if (has_two_ops) {
        if (syntax && strcmp(syntax, "att") == 0) {
            src = op1;
            dest = op2;
        } else {
            dest = op1;
            src = op2;
        }
    }
    
    /* Pattern 1: mov rax, rax -> remove */
    if ((strcmp(base_mnemonic, "mov") == 0 || strcmp(mnemonic_lower, "mov") == 0) && has_two_ops) {
        bool dest_reg = asmopt_is_register(dest, syntax);
        bool src_reg = asmopt_is_register(src, syntax);
        if (dest_reg && src_reg && asmopt_casecmp(dest, src) == 0) {
            asmopt_record_optimization(ctx, line_no, "redundant_mov", line, NULL);
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
                snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s", indent, xor_name, spacing, dest, pre_space, post_space, dest);
                if (!asmopt_is_blank(trimmed_comment)) {
                    strcat(newline, " ");
                    strcat(newline, trimmed_comment);
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
    
    /* Pattern 3: imul/mul rax, 1 -> remove (identity) */
    if ((strcmp(base_mnemonic, "imul") == 0 || strcmp(base_mnemonic, "mul") == 0) && has_two_ops) {
        if (asmopt_is_immediate_one(src, syntax)) {
            asmopt_record_optimization(ctx, line_no, "mul_by_one", line, NULL);
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
            goto cleanup;
        }
        
        /* Pattern 4: imul rax, power_of_2 -> shl rax, log2(power_of_2) */
        bool success = false;
        long imm_val = asmopt_parse_immediate(src, syntax, &success);
        if (success && asmopt_is_power_of_two(imm_val)) {
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
                snprintf(newline, new_len + 1, "%s%s%s%s%s,%s%s", indent, shl_name, spacing, dest, pre_space, post_space, shift_str);
                if (!asmopt_is_blank(trimmed_comment)) {
                    strcat(newline, " ");
                    strcat(newline, trimmed_comment);
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
        if (asmopt_is_immediate_zero(src, syntax)) {
            asmopt_record_optimization(ctx, line_no, "add_sub_zero", line, NULL);
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
            goto cleanup;
        }
    }
    
    /* Pattern 6: shl/shr rax, 0 -> remove (identity) */
    if ((strcmp(base_mnemonic, "shl") == 0 || strcmp(base_mnemonic, "shr") == 0 || 
         strcmp(base_mnemonic, "sal") == 0 || strcmp(base_mnemonic, "sar") == 0) && has_two_ops) {
        if (asmopt_is_immediate_zero(src, syntax)) {
            asmopt_record_optimization(ctx, line_no, "shift_by_zero", line, NULL);
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
    bool do_opt = asmopt_should_optimize(ctx);
    for (size_t i = 0; i < ctx->original_count; i++) {
        if (!do_opt) {
            asmopt_store_optimized_line(ctx, ctx->original_lines[i]);
            continue;
        }
        bool replaced = false;
        bool removed = false;
        asmopt_peephole_line(ctx, i + 1, ctx->original_lines[i], syntax, &replaced, &removed);
        if (replaced) {
            ctx->stats.replacements += 1;
        }
        if (removed) {
            ctx->stats.removals += 1;
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
    
    int offset = snprintf(buffer, buffer_size,
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
    
    if (ctx->opt_event_count > 0) {
        offset += snprintf(buffer + offset, buffer_size - offset, 
                          "\nOptimizations Applied:\n");
        
        for (size_t i = 0; i < ctx->opt_event_count; i++) {
            asmopt_optimization_event* event = &ctx->opt_events[i];
            offset += snprintf(buffer + offset, buffer_size - offset,
                             "  Line %zu: %s\n"
                             "    Before: %s\n"
                             "    After:  %s\n",
                             event->line_no,
                             event->pattern_name,
                             event->original,
                             event->optimized);
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
    for (size_t i = 0; i < ctx->opt_event_count; i++) {
        free(ctx->opt_events[i].pattern_name);
        free(ctx->opt_events[i].original);
        free(ctx->opt_events[i].optimized);
    }
    free(ctx->opt_events);
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
