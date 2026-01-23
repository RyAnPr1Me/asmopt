/*
 * Integration tests for the complete asmopt pipeline
 */

#include "../include/asmopt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Integration test: Complete function optimization */
static int test_complete_function() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    asmopt_set_option(ctx, "hot_align", "1");
    
    const char* input = 
        ".text\n"
        ".globl optimize_me\n"
        "optimize_me:\n"
        "    mov rax, rax     ; redundant\n"
        "    mov rbx, 0       ; should be xor\n"
        "    imul rcx, 1      ; identity\n"
        "    imul rdx, 16     ; power of 2\n"
        "    add rsi, 0       ; identity\n"
        "    shl rdi, 0       ; identity\n"
        "    or r8, 0         ; identity\n"
        "    xor r9, 0        ; identity (not xor reg,reg)\n"
        "    and r10, -1      ; identity\n"
        "    and r14, 0       ; zero idiom\n"
        "    add r11, 1       ; should become inc\n"
        "    sub r12, 1       ; should become dec\n"
        "    cmp rsi, 0       ; zero compare\n"
        "    or rdi, rdi      ; flag-only\n"
        "    add r8, -1       ; negative add\n"
        "    sub r9, -1       ; negative sub\n"
        "    and rdx, rdx     ; flag-only\n"
        "    cmp rcx, rcx     ; self-compare\n"
        "    jmp .fallthrough\n"
        ".fallthrough:\n"
        ".hot_loop:\n"
        "    mov r13, r14     ; swap 1\n"
        "    mov r14, r13     ; swap 2\n"
        "    sub rax, rax     ; zero idiom\n"
        "    xor r15, r15     ; zero idiom - keep\n"
        "    mov rbx, 42      ; keep\n"
        "    ret\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    
    /* Verify directives and labels preserved */
    TEST_ASSERT(strstr(output, ".text") != NULL, "Directive removed");
    TEST_ASSERT(strstr(output, ".globl optimize_me") != NULL, "Directive removed");
    TEST_ASSERT(strstr(output, "optimize_me:") != NULL, "Label removed");
    
    /* Verify optimizations applied */
    TEST_ASSERT(strstr(output, "mov rax, rax") == NULL, "Redundant mov not removed");
    TEST_ASSERT(strstr(output, "imul rcx, 1") == NULL, "Multiply by 1 not removed");
    TEST_ASSERT(strstr(output, "shl rdx, 4") != NULL, "Power of 2 multiply not converted");
    TEST_ASSERT(strstr(output, "add rsi, 0") == NULL, "Add zero not removed");
    TEST_ASSERT(strstr(output, "shl rdi, 0") == NULL, "Shift zero not removed");
    TEST_ASSERT(strstr(output, "or r8, 0") == NULL, "OR zero not removed");
    TEST_ASSERT(strstr(output, "xor r9, 0") == NULL, "XOR zero immediate not removed");
    TEST_ASSERT(strstr(output, "and r10, -1") == NULL, "AND -1 not removed");
    TEST_ASSERT(strstr(output, "xor r14, r14") != NULL, "AND zero not converted to xor");
    TEST_ASSERT(strstr(output, "xor rax, rax") != NULL, "sub self not converted to xor");
    TEST_ASSERT(strstr(output, "test rsi, rsi") != NULL, "cmp zero not converted to test");
    TEST_ASSERT(strstr(output, "test rdi, rdi") != NULL, "or self not converted to test");
    TEST_ASSERT(strstr(output, "dec r8") != NULL, "add -1 not converted to dec");
    TEST_ASSERT(strstr(output, "inc r9") != NULL, "sub -1 not converted to inc");
    TEST_ASSERT(strstr(output, "test rdx, rdx") != NULL, "and self not converted to test");
    TEST_ASSERT(strstr(output, "test rcx, rcx") != NULL, "cmp self not converted to test");
    TEST_ASSERT(strstr(output, "jmp .fallthrough") == NULL, "Fallthrough jump not removed");
    TEST_ASSERT(strstr(output, "inc r11") != NULL, "add 1 not converted to inc");
    TEST_ASSERT(strstr(output, "dec r12") != NULL, "sub 1 not converted to dec");
    TEST_ASSERT(strstr(output, ".align 64") != NULL, "Hot loop not aligned");
    TEST_ASSERT(strstr(output, "mov r13, r14") != NULL, "Swap move not preserved");
    
    /* Verify non-optimizable kept */
    TEST_ASSERT(strstr(output, "xor r15, r15") != NULL, "Zero idiom removed");
    TEST_ASSERT(strstr(output, "mov rbx, 42") != NULL, "Valid mov removed");
    TEST_ASSERT(strstr(output, "ret") != NULL, "Return removed");
    
    size_t original, optimized, replacements, removals;
    asmopt_get_stats(ctx, &original, &optimized, &replacements, &removals);
    /* Replacements unchanged: hot loop alignment is an insertion. */
    TEST_ASSERT(replacements == 13, "Expected 13 replacements");
    /* 9 removals: redundant mov, imul-by-1, add/sub zero, shift zero, or zero, xor zero,
       and -1, fallthrough jump */
    TEST_ASSERT(removals == 9, "Expected 9 removals");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_complete_function");
}

/* Test file-based I/O */
static int test_file_io() {
    const char* test_file = "/tmp/test_asmopt_io.s";
    const char* output_file = "/tmp/test_asmopt_out.s";
    
    /* Create test file */
    FILE* f = fopen(test_file, "w");
    TEST_ASSERT(f != NULL, "Failed to create test file");
    fprintf(f, "mov rax, 0\nimul rbx, 8\n");
    fclose(f);
    
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    asmopt_set_option(ctx, "hot_align", "1");
    
    int result = asmopt_parse_file(ctx, test_file);
    TEST_ASSERT(result == 0, "Failed to parse file");
    
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    
    /* Write output */
    f = fopen(output_file, "w");
    TEST_ASSERT(f != NULL, "Failed to create output file");
    fputs(output, f);
    fclose(f);
    
    /* Verify output file contains optimizations */
    f = fopen(output_file, "r");
    TEST_ASSERT(f != NULL, "Failed to open output file");
    
    char buffer[1024];
    size_t read_size = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[read_size] = '\0';
    fclose(f);
    
    TEST_ASSERT(strstr(buffer, "xor rax, rax") != NULL, "Optimization not in file");
    TEST_ASSERT(strstr(buffer, "shl rbx, 3") != NULL, "Optimization not in file");
    
    /* Cleanup */
    remove(test_file);
    remove(output_file);
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_file_io");
}

/* Test optimization levels */
static int test_optimization_levels() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    /* Test level 0 (no optimization) */
    asmopt_set_optimization_level(ctx, 0);
    const char* input = "mov rax, 0\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(strstr(output, "mov rax, 0") != NULL, "Level 0 applied optimization");
    free(output);
    
    /* Reset and test level 2 */
    asmopt_destroy(ctx);
    ctx = asmopt_create("x86-64");
    asmopt_set_optimization_level(ctx, 2);
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(strstr(output, "xor rax, rax") != NULL, "Level 2 did not optimize");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_optimization_levels");
}

/* Test option setting */
static int test_option_setting() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    asmopt_set_option(ctx, "test_key", "test_value");
    asmopt_set_target_cpu(ctx, "zen3");
    asmopt_set_format(ctx, "intel");
    asmopt_set_amd_optimizations(ctx, 1);
    
    /* These should not crash */
    asmopt_enable_optimization(ctx, "peephole");
    asmopt_disable_optimization(ctx, "peephole");
    asmopt_enable_optimization(ctx, "all");
    
    asmopt_destroy(ctx);
    TEST_PASS("test_option_setting");
}

/* Test IR and CFG dump functions */
static int test_ir_cfg_dump() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = 
        "test_func:\n"
        "    mov rax, 0\n"
        "    test rax, rax\n"
        "    jz .label\n"
        "    ret\n"
        ".label:\n"
        "    mov rbx, 1\n"
        "    ret\n";
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* ir = asmopt_dump_ir_text(ctx);
    TEST_ASSERT(ir != NULL, "Failed to dump IR");
    TEST_ASSERT(strlen(ir) > 0, "IR dump is empty");
    free(ir);
    
    char* cfg_text = asmopt_dump_cfg_text(ctx);
    TEST_ASSERT(cfg_text != NULL, "Failed to dump CFG text");
    TEST_ASSERT(strlen(cfg_text) > 0, "CFG text is empty");
    free(cfg_text);
    
    char* cfg_dot = asmopt_dump_cfg_dot(ctx);
    TEST_ASSERT(cfg_dot != NULL, "Failed to dump CFG dot");
    TEST_ASSERT(strlen(cfg_dot) > 0, "CFG dot is empty");
    free(cfg_dot);
    
    asmopt_destroy(ctx);
    TEST_PASS("test_ir_cfg_dump");
}

/* Test large input handling */
static int test_large_input() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    /* Create a large input with many instructions */
    char* large_input = malloc(100000);
    TEST_ASSERT(large_input != NULL, "Failed to allocate memory");
    
    char* ptr = large_input;
    for (int i = 0; i < 1000; i++) {
        ptr += sprintf(ptr, "mov rax, rax\nmov rbx, 0\nadd rcx, 0\n");
    }
    
    asmopt_parse_string(ctx, large_input);
    asmopt_optimize(ctx);
    
    size_t original, optimized, replacements, removals;
    asmopt_get_stats(ctx, &original, &optimized, &replacements, &removals);
    
    /* Just verify we got some optimizations, don't check exact counts */
    TEST_ASSERT(original > 0, "No original lines");
    TEST_ASSERT(replacements > 0, "No replacements");
    TEST_ASSERT(removals > 0, "No removals");
    
    free(large_input);
    asmopt_destroy(ctx);
    TEST_PASS("test_large_input");
}

/* Test edge cases */
static int test_edge_cases() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    /* Empty input */
    asmopt_parse_string(ctx, "");
    asmopt_optimize(ctx);
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Empty input failed");
    free(output);
    
    /* Only comments */
    asmopt_parse_string(ctx, "; comment only\n");
    asmopt_optimize(ctx);
    output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Comment-only input failed");
    TEST_ASSERT(strstr(output, "comment only") != NULL, "Comment lost");
    free(output);
    
    /* Only whitespace */
    asmopt_parse_string(ctx, "   \n\t\n  \n");
    asmopt_optimize(ctx);
    output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Whitespace-only input failed");
    free(output);
    
    asmopt_destroy(ctx);
    TEST_PASS("test_edge_cases");
}

/* Test report with all pattern types */
static int test_comprehensive_report() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    asmopt_set_option(ctx, "hot_align", "1");
    
    const char* input = 
        "mov rax, rax\n"    /* Pattern 1 */
        "mov rbx, 0\n"      /* Pattern 2 */
        "imul rcx, 1\n"     /* Pattern 3 */
        "imul rdx, 4\n"     /* Pattern 4 */
        "add rsi, 0\n"      /* Pattern 5 */
        "shl rdi, 0\n"      /* Pattern 6 */
        "or r8, 0\n"        /* Pattern 7 */
        "xor r9, 0\n"       /* Pattern 8 */
        "and r10, -1\n"     /* Pattern 9 */
        "add r11, 1\n"      /* Pattern 10 */
        "sub r12, 1\n"      /* Pattern 11 */
        "mov r13, r14\n"    /* Pattern 12 */
        "mov r14, r13\n"    /* Pattern 12 */
        "sub r15, r15\n"    /* Pattern 13 */
        "and rax, 0\n"      /* Pattern 14 */
        "cmp rbx, 0\n"      /* Pattern 15 */
        "or rcx, rcx\n"     /* Pattern 16 */
        "add rdx, -1\n"     /* Pattern 17 */
        "sub rsi, -1\n"     /* Pattern 18 */
        "and r8, r8\n"      /* Pattern 19 */
        "cmp r9, r9\n"      /* Pattern 20 */
        "jmp .fall\n"
        ".fall:\n"          /* Pattern 21 */
        ".hot_loop:\n";     /* Pattern 22 */
    
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* report = asmopt_generate_report(ctx);
    TEST_ASSERT(report != NULL, "Failed to generate report");
    
    /* Verify all patterns in report */
    TEST_ASSERT(strstr(report, "redundant_mov") != NULL, "Pattern 1 missing");
    TEST_ASSERT(strstr(report, "mov_zero_to_xor") != NULL, "Pattern 2 missing");
    TEST_ASSERT(strstr(report, "mul_by_one") != NULL, "Pattern 3 missing");
    TEST_ASSERT(strstr(report, "mul_power_of_2_to_shift") != NULL, "Pattern 4 missing");
    TEST_ASSERT(strstr(report, "add_sub_zero") != NULL, "Pattern 5 missing");
    TEST_ASSERT(strstr(report, "shift_by_zero") != NULL, "Pattern 6 missing");
    TEST_ASSERT(strstr(report, "or_zero") != NULL, "Pattern 7 missing");
    TEST_ASSERT(strstr(report, "xor_zero") != NULL, "Pattern 8 missing");
    TEST_ASSERT(strstr(report, "and_minus_one") != NULL, "Pattern 9 missing");
    TEST_ASSERT(strstr(report, "add_one_to_inc") != NULL, "Pattern 10 missing");
    TEST_ASSERT(strstr(report, "sub_one_to_dec") != NULL, "Pattern 11 missing");
    TEST_ASSERT(strstr(report, "redundant_move_pair") != NULL, "Pattern 12 missing");
    TEST_ASSERT(strstr(report, "sub_self_to_xor") != NULL, "Pattern 13 missing");
    TEST_ASSERT(strstr(report, "and_zero_to_xor") != NULL, "Pattern 14 missing");
    TEST_ASSERT(strstr(report, "cmp_zero_to_test") != NULL, "Pattern 15 missing");
    TEST_ASSERT(strstr(report, "or_self_to_test") != NULL, "Pattern 16 missing");
    TEST_ASSERT(strstr(report, "add_minus_one_to_dec") != NULL, "Pattern 17 missing");
    TEST_ASSERT(strstr(report, "sub_minus_one_to_inc") != NULL, "Pattern 18 missing");
    TEST_ASSERT(strstr(report, "and_self_to_test") != NULL, "Pattern 19 missing");
    TEST_ASSERT(strstr(report, "cmp_self_to_test") != NULL, "Pattern 20 missing");
    TEST_ASSERT(strstr(report, "fallthrough_jump") != NULL, "Pattern 21 missing");
    TEST_ASSERT(strstr(report, "hot_loop_align") != NULL, "Pattern 22 missing");
    
    /* 13 replacements correspond to the same list in test_complete_function above. */
    TEST_ASSERT(strstr(report, "Replacements: 13") != NULL, "Wrong replacement count");
    TEST_ASSERT(strstr(report, "Removals: 9") != NULL, "Wrong removal count");
    
    free(report);
    asmopt_destroy(ctx);
    TEST_PASS("test_comprehensive_report");
}

int main() {
    int passed = 0;
    int total = 0;
    
    printf("Running asmopt integration tests...\n\n");
    
    total++; passed += test_complete_function();
    total++; passed += test_file_io();
    total++; passed += test_optimization_levels();
    total++; passed += test_option_setting();
    total++; passed += test_ir_cfg_dump();
    total++; passed += test_large_input();
    total++; passed += test_edge_cases();
    total++; passed += test_comprehensive_report();
    
    printf("\n========================================\n");
    printf("Test Results: %d/%d tests passed\n", passed, total);
    printf("========================================\n");
    
    return (passed == total) ? 0 : 1;
}
