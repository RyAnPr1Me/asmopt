/*
 * Comprehensive API and edge case tests for asmopt
 * Tests all public API functions and edge cases
 */

#include "../include/asmopt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/asmopt.h"

#define TEST_ASSERT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        return 0; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    fprintf(stdout, "PASS: %s\n", name); \
    return 1; \
} while(0)

/* Test context creation with different architectures */
static int test_create_with_architectures() {
    asmopt_context* ctx1 = asmopt_create("x86");
    TEST_ASSERT(ctx1 != NULL, "Failed to create x86 context");
    asmopt_destroy(ctx1);
    
    asmopt_context* ctx2 = asmopt_create("x86-64");
    TEST_ASSERT(ctx2 != NULL, "Failed to create x86-64 context");
    asmopt_destroy(ctx2);
    
    TEST_PASS("test_create_with_architectures");
}

/* Test all optimization level settings */
static int test_all_optimization_levels() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "mov rax, 0\n";
    
    for (int level = 0; level <= 4; level++) {
        asmopt_set_optimization_level(ctx, level);
        asmopt_parse_string(ctx, input);
        asmopt_optimize(ctx);
        char* output = asmopt_generate_assembly(ctx);
        TEST_ASSERT(output != NULL, "Failed to generate assembly");
        free(output);
    }
    
    asmopt_destroy(ctx);
    TEST_PASS("test_all_optimization_levels");
}

/* Test all target CPU settings */
static int test_target_cpu_settings() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* cpus[] = {"generic", "zen", "zen2", "zen3", "zen4"};
    for (int i = 0; i < 5; i++) {
        asmopt_set_target_cpu(ctx, cpus[i]);
    }
    
    asmopt_destroy(ctx);
    TEST_PASS("test_target_cpu_settings");
}

/* Test format settings */
static int test_format_settings() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    asmopt_set_format(ctx, "intel");
    asmopt_set_format(ctx, "att");
    
    asmopt_destroy(ctx);
    TEST_PASS("test_format_settings");
}

/* Test enable/disable optimizations */
static int test_enable_disable_opts() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    asmopt_enable_optimization(ctx, "peephole");
    asmopt_enable_optimization(ctx, "dead_code");
    asmopt_disable_optimization(ctx, "peephole");
    asmopt_disable_optimization(ctx, "dead_code");
    
    asmopt_destroy(ctx);
    TEST_PASS("test_enable_disable_opts");
}

/* Test no-optimize flag */
static int test_no_optimize_flag() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    asmopt_set_no_optimize(ctx, 1);
    const char* input = "mov rax, 0\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "mov rax, 0") != NULL, "Optimization was applied despite no-optimize");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_no_optimize_flag");
}

/* Test preserve-all flag */
static int test_preserve_all_flag() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    asmopt_set_preserve_all(ctx, 1);
    const char* input = "  mov rax, 0  ; comment\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_preserve_all_flag");
}

/* Test AMD optimizations flag */
static int test_amd_optimizations() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    asmopt_set_amd_optimizations(ctx, 1);
    asmopt_set_amd_optimizations(ctx, 0);
    
    asmopt_destroy(ctx);
    TEST_PASS("test_amd_optimizations");
}

/* Test generic option setting */
static int test_generic_options() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    asmopt_set_option(ctx, "test_key1", "value1");
    asmopt_set_option(ctx, "test_key2", "value2");
    
    asmopt_destroy(ctx);
    TEST_PASS("test_generic_options");
}

/* Test parse from file */
static int test_parse_from_file() {
    const char* test_file = "/tmp/test_parse_file.s";
    FILE* f = fopen(test_file, "w");
    TEST_ASSERT(f != NULL, "Failed to create test file");
    fprintf(f, "mov rax, 0\n");
    fclose(f);
    
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    int result = asmopt_parse_file(ctx, test_file);
    TEST_ASSERT(result == 0, "Failed to parse file");
    
    remove(test_file);
    asmopt_destroy(ctx);
    TEST_PASS("test_parse_from_file");
}

/* Test statistics function */
static int test_get_statistics() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "mov rax, rax\nmov rbx, 0\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    size_t original, optimized, replacements, removals;
    asmopt_get_stats(ctx, &original, &optimized, &replacements, &removals);
    
    TEST_ASSERT(original > 0, "Original count is zero");
    TEST_ASSERT(replacements == 1, "Expected 1 replacement");
    TEST_ASSERT(removals == 1, "Expected 1 removal");
    
    asmopt_destroy(ctx);
    TEST_PASS("test_get_statistics");
}

/* Test IR dump */
static int test_ir_dump() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "mov rax, 0\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* ir = asmopt_dump_ir_text(ctx);
    TEST_ASSERT(ir != NULL, "Failed to dump IR");
    TEST_ASSERT(strlen(ir) > 0, "IR is empty");
    
    free(ir);
    asmopt_destroy(ctx);
    TEST_PASS("test_ir_dump");
}

/* Test CFG dump (text) */
static int test_cfg_dump_text() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "main:\nmov rax, 0\nret\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* cfg = asmopt_dump_cfg_text(ctx);
    TEST_ASSERT(cfg != NULL, "Failed to dump CFG");
    TEST_ASSERT(strlen(cfg) > 0, "CFG is empty");
    
    free(cfg);
    asmopt_destroy(ctx);
    TEST_PASS("test_cfg_dump_text");
}

/* Test CFG dump (dot format) */
static int test_cfg_dump_dot() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "main:\nmov rax, 0\nret\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* dot = asmopt_dump_cfg_dot(ctx);
    TEST_ASSERT(dot != NULL, "Failed to dump CFG dot");
    TEST_ASSERT(strlen(dot) > 0, "CFG dot is empty");
    
    free(dot);
    asmopt_destroy(ctx);
    TEST_PASS("test_cfg_dump_dot");
}

/* Test multiple directives */
static int test_multiple_directives() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = 
        ".section .text\n"
        ".align 16\n"
        ".globl main\n"
        ".type main, @function\n"
        "main:\n"
        "    mov rax, 0\n"
        "    ret\n"
        ".size main, .-main\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, ".section") != NULL, "Directive lost");
    TEST_ASSERT(strstr(output, ".align") != NULL, "Directive lost");
    TEST_ASSERT(strstr(output, ".globl") != NULL, "Directive lost");
    TEST_ASSERT(strstr(output, "main:") != NULL, "Label lost");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_multiple_directives");
}

/* Test complex control flow */
static int test_complex_control_flow() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "main:\n"
        "    cmp rax, rbx\n"
        "    je .equal\n"
        "    jl .less\n"
        "    mov rcx, 1\n"
        "    jmp .end\n"
        ".equal:\n"
        "    mov rcx, 0\n"
        "    jmp .end\n"
        ".less:\n"
        "    mov rcx, -1\n"
        ".end:\n"
        "    ret\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, ".equal:") != NULL, "Label lost");
    TEST_ASSERT(strstr(output, ".less:") != NULL, "Label lost");
    TEST_ASSERT(strstr(output, ".end:") != NULL, "Label lost");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_complex_control_flow");
}

/* Test whitespace and blank lines */
static int test_whitespace_handling() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "\n\n"
        "    \n"
        "mov rax, 0\n"
        "\n"
        "    \t\n"
        "mov rbx, 5\n"
        "\n\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_whitespace_handling");
}

/* Test very long lines */
static int test_long_lines() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    char long_comment[1024];
    for (int i = 0; i < 500; i++) {
        long_comment[i] = 'x';
    }
    long_comment[500] = '\0';
    
    char input[2048];
    snprintf(input, sizeof(input), "mov rax, 0 ; %s\n", long_comment);
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_long_lines");
}

/* Test all register combinations for patterns */
static int test_all_registers() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* regs[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    
    for (int i = 0; i < 14; i++) {
        char input[256];
        snprintf(input, sizeof(input), "mov %s, 0\n", regs[i]);
        asmopt_parse_string(ctx, input);
        asmopt_optimize(ctx);
        
        char* output = asmopt_generate_assembly(ctx);
        TEST_ASSERT(output != NULL, "Failed to generate output");
        
        char expected[256];
        snprintf(expected, sizeof(expected), "xor %s, %s", regs[i], regs[i]);
        TEST_ASSERT(strstr(output, expected) != NULL, "Optimization not applied to register");
        
        free(output);
    }
    
    asmopt_destroy(ctx);
    TEST_PASS("test_all_registers");
}

/* Test hex immediates */
static int test_hex_immediates() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "mov rax, 0x0\n"
        "imul rbx, 0x8\n"
        "add rcx, 0x1\n"
        "and rdx, 0xFFFFFFFFFFFFFFFF\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "xor rax, rax") != NULL, "Hex 0 not optimized");
    TEST_ASSERT(strstr(output, "shl rbx, 3") != NULL, "Hex power of 2 not optimized");
    TEST_ASSERT(strstr(output, "inc rcx") != NULL, "Hex 1 not optimized");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_hex_immediates");
}

/* Test memory operands (should not be optimized) */
static int test_memory_operands() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "mov qword [rax], 0\n"
        "add qword [rbx], 0\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    // Memory operands should not be converted
    TEST_ASSERT(strstr(output, "[rax]") != NULL, "Memory operand modified");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_memory_operands");
}

/* Test all power of 2 values */
static int test_all_powers_of_2() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    int powers[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    int shifts[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    
    for (int i = 1; i < 11; i++) {  // Skip 1 as it's handled by mul_by_one
        char input[256];
        snprintf(input, sizeof(input), "imul rax, %d\n", powers[i]);
        asmopt_parse_string(ctx, input);
        asmopt_optimize(ctx);
        
        char* output = asmopt_generate_assembly(ctx);
        TEST_ASSERT(output != NULL, "Failed to generate output");
        
        char expected[256];
        snprintf(expected, sizeof(expected), "shl rax, %d", shifts[i]);
        TEST_ASSERT(strstr(output, expected) != NULL, "Power of 2 not converted to shift");
        
        free(output);
    }
    
    asmopt_destroy(ctx);
    TEST_PASS("test_all_powers_of_2");
}

/* Test swap move elimination */
static int test_swap_move_optimization() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "mov rax, rbx\n"
        "mov rbx, rax\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "mov rax, rbx") != NULL, "Swap move not preserved");
    TEST_ASSERT(strstr(output, "mov rbx, rax") == NULL, "Second mov not removed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_swap_move_optimization");
}

/* Test sub self optimization */
static int test_sub_self_optimization() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "sub rax, rax\n"
        "sub rbx, rcx\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "xor rax, rax") != NULL, "sub self not converted to xor");
    TEST_ASSERT(strstr(output, "sub rbx, rcx") != NULL, "Non-self sub was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_sub_self_optimization");
}

/* Test and zero optimization */
static int test_and_zero_optimization() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "and rax, 0\n"
        "and rbx, 7\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "xor rax, rax") != NULL, "and 0 not converted to xor");
    TEST_ASSERT(strstr(output, "and rbx, 7") != NULL, "Non-zero and was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_and_zero_optimization");
}

/* Test cmp zero optimization */
static int test_cmp_zero_optimization() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "cmp rax, 0\n"
        "cmp rbx, 3\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "test rax, rax") != NULL, "cmp 0 not converted to test");
    TEST_ASSERT(strstr(output, "cmp rbx, 3") != NULL, "Non-zero cmp was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_cmp_zero_optimization");
}

/* Test or self optimization */
static int test_or_self_optimization() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "or rax, rax\n"
        "or rbx, rcx\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "test rax, rax") != NULL, "or self not converted to test");
    TEST_ASSERT(strstr(output, "or rbx, rcx") != NULL, "Non-self or was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_or_self_optimization");
}

/* Test add -1 optimization */
static int test_add_minus_one_optimization() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "add rax, -1\n"
        "add rbx, 4\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "dec rax") != NULL, "add -1 not converted to dec");
    TEST_ASSERT(strstr(output, "add rbx, 4") != NULL, "Non -1 add was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_add_minus_one_optimization");
}

/* Test sub -1 optimization */
static int test_sub_minus_one_optimization() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "sub rax, -1\n"
        "sub rbx, 6\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "inc rax") != NULL, "sub -1 not converted to inc");
    TEST_ASSERT(strstr(output, "sub rbx, 6") != NULL, "Non -1 sub was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_sub_minus_one_optimization");
}

/* Test and self optimization */
static int test_and_self_optimization() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "and rax, rax\n"
        "and rbx, rcx\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "test rax, rax") != NULL, "and self not converted to test");
    TEST_ASSERT(strstr(output, "and rbx, rcx") != NULL, "Non-self and was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_and_self_optimization");
}

/* Test cmp self optimization */
static int test_cmp_self_optimization() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "cmp rax, rax\n"
        "cmp rbx, rcx\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "test rax, rax") != NULL, "cmp self not converted to test");
    TEST_ASSERT(strstr(output, "cmp rbx, rcx") != NULL, "Non-self cmp was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_cmp_self_optimization");
}

/* Test fallthrough jump optimization */
static int test_fallthrough_jump_optimization() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input =
        "jmp .target\n"
        ".target:\n"
        "mov rax, 0\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "jmp .target") == NULL, "Fallthrough jump not removed");
    TEST_ASSERT(strstr(output, ".target:") != NULL, "Target label removed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_fallthrough_jump_optimization");
}

/* Test hot loop alignment */
static int test_hot_loop_alignment() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    asmopt_set_option(ctx, "hot_align", "1");
    const char* input =
        ".hot_loop:\n"
        "add rax, 1\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    char expected[32];
    snprintf(expected, sizeof(expected), ".align %d", ASMOPT_HOT_LOOP_ALIGNMENT);
    TEST_ASSERT(strstr(output, expected) != NULL, "Alignment directive missing");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_hot_loop_alignment");
}

/* Test bsf to tzcnt optimization on Zen */
static int test_bsf_to_tzcnt() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    asmopt_set_target_cpu(ctx, "zen4");
    
    const char* input =
        "test rbx, rbx\n"
        "jz .skip\n"
        "bsf rax, rbx\n"
        ".skip:\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "tzcnt rax, rbx") != NULL, "bsf not converted to tzcnt");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_bsf_to_tzcnt");
}

/* Test Pattern 26 removed: bsr -> lzcnt not applied */

int main() {
    int passed = 0;
    int total = 0;
    
    printf("Running comprehensive API and edge case tests...\n\n");
    
    total++; passed += test_create_with_architectures();
    total++; passed += test_all_optimization_levels();
    total++; passed += test_target_cpu_settings();
    total++; passed += test_format_settings();
    total++; passed += test_enable_disable_opts();
    total++; passed += test_no_optimize_flag();
    total++; passed += test_preserve_all_flag();
    total++; passed += test_amd_optimizations();
    total++; passed += test_generic_options();
    total++; passed += test_parse_from_file();
    total++; passed += test_get_statistics();
    total++; passed += test_ir_dump();
    total++; passed += test_cfg_dump_text();
    total++; passed += test_cfg_dump_dot();
    total++; passed += test_multiple_directives();
    total++; passed += test_complex_control_flow();
    total++; passed += test_whitespace_handling();
    total++; passed += test_long_lines();
    total++; passed += test_all_registers();
    total++; passed += test_hex_immediates();
    total++; passed += test_memory_operands();
    total++; passed += test_all_powers_of_2();
    total++; passed += test_swap_move_optimization();
    total++; passed += test_sub_self_optimization();
    total++; passed += test_and_zero_optimization();
    total++; passed += test_cmp_zero_optimization();
    total++; passed += test_or_self_optimization();
    total++; passed += test_add_minus_one_optimization();
    total++; passed += test_sub_minus_one_optimization();
    total++; passed += test_and_self_optimization();
    total++; passed += test_cmp_self_optimization();
    total++; passed += test_fallthrough_jump_optimization();
    total++; passed += test_hot_loop_alignment();
    total++; passed += test_bsf_to_tzcnt();
    
    printf("\n========================================\n");
    printf("Test Results: %d/%d tests passed\n", passed, total);
    printf("========================================\n");
    
    return (passed == total) ? 0 : 1;
}
