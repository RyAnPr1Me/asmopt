/*
 * Unit tests for peephole optimization patterns
 */

#include "../include/asmopt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
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

/* Test Pattern 1: Redundant mov elimination */
static int test_redundant_mov() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "mov rax, rax\nmov rbx, rcx\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "mov rax, rax") == NULL, "Redundant mov not removed");
    TEST_ASSERT(strstr(output, "mov rbx, rcx") != NULL, "Valid mov was removed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_redundant_mov");
}

/* Test Pattern 2: mov zero to xor */
static int test_mov_zero_to_xor() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "mov rax, 0\nmov rbx, 5\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "xor rax, rax") != NULL, "mov 0 not converted to xor");
    TEST_ASSERT(strstr(output, "mov rbx, 5") != NULL, "mov with non-zero was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_mov_zero_to_xor");
}

/* Test Pattern 3: Multiply by one elimination */
static int test_mul_by_one() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "imul rax, 1\nimul rbx, 2\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "imul rax, 1") == NULL, "Multiply by 1 not removed");
    TEST_ASSERT(strstr(output, "imul rbx, 2") != NULL || strstr(output, "shl rbx") != NULL, 
                "Valid multiply was incorrectly removed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_mul_by_one");
}

/* Test Pattern 4: Power of 2 multiply to shift */
static int test_mul_power_of_2() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "imul rax, 8\nimul rbx, 3\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "shl rax, 3") != NULL, "Power of 2 multiply not converted to shift");
    TEST_ASSERT(strstr(output, "imul rbx, 3") != NULL, "Non-power-of-2 multiply was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_mul_power_of_2");
}

/* Test Pattern 5: Add/sub zero elimination */
static int test_add_sub_zero() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "add rax, 0\nsub rbx, 0\nadd rcx, 5\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "add rax, 0") == NULL, "Add zero not removed");
    TEST_ASSERT(strstr(output, "sub rbx, 0") == NULL, "Sub zero not removed");
    TEST_ASSERT(strstr(output, "add rcx, 5") != NULL, "Valid add was removed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_add_sub_zero");
}

/* Test Pattern 6: Shift by zero elimination */
static int test_shift_zero() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "shl rax, 0\nshr rbx, 0\nshl rcx, 3\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "shl rax, 0") == NULL, "Shift by zero not removed");
    TEST_ASSERT(strstr(output, "shr rbx, 0") == NULL, "Shift by zero not removed");
    TEST_ASSERT(strstr(output, "shl rcx, 3") != NULL, "Valid shift was removed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_shift_zero");
}

/* Test Pattern 7: OR zero elimination */
static int test_or_zero() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "or rax, 0\nor rbx, 5\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "or rax, 0") == NULL, "OR zero not removed");
    TEST_ASSERT(strstr(output, "or rbx, 5") != NULL, "Valid OR was removed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_or_zero");
}

/* Test Pattern 8: XOR zero immediate elimination (preserve xor reg,reg) */
static int test_xor_zero() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "xor rax, 0\nxor rbx, rbx\nxor rcx, 5\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "xor rax, 0") == NULL, "XOR with immediate zero not removed");
    TEST_ASSERT(strstr(output, "xor rbx, rbx") != NULL, "Zero idiom xor reg,reg was removed");
    TEST_ASSERT(strstr(output, "xor rcx, 5") != NULL, "Valid XOR was removed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_xor_zero");
}

/* Test optimization statistics */
static int test_optimization_stats() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "mov rax, rax\nmov rbx, 0\nadd rcx, 0\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    size_t original, optimized, replacements, removals;
    asmopt_get_stats(ctx, &original, &optimized, &replacements, &removals);
    
    TEST_ASSERT(original > 0, "Original line count is zero");
    TEST_ASSERT(replacements == 1, "Expected 1 replacement (mov 0 -> xor)");
    TEST_ASSERT(removals == 2, "Expected 2 removals (mov rax,rax and add rcx,0)");
    
    asmopt_destroy(ctx);
    TEST_PASS("test_optimization_stats");
}

/* Test report generation */
static int test_report_generation() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "mov rax, 0\nimul rbx, 8\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* report = asmopt_generate_report(ctx);
    TEST_ASSERT(report != NULL, "Failed to generate report");
    TEST_ASSERT(strstr(report, "Optimization Report") != NULL, "Report missing header");
    TEST_ASSERT(strstr(report, "mov_zero_to_xor") != NULL, "Report missing pattern name");
    TEST_ASSERT(strstr(report, "mul_power_of_2_to_shift") != NULL, "Report missing pattern name");
    
    free(report);
    asmopt_destroy(ctx);
    TEST_PASS("test_report_generation");
}

/* Test context creation and destruction */
static int test_context_lifecycle() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    asmopt_set_optimization_level(ctx, 3);
    asmopt_set_target_cpu(ctx, "zen3");
    asmopt_enable_optimization(ctx, "peephole");
    
    asmopt_destroy(ctx);
    TEST_PASS("test_context_lifecycle");
}

/* Test with comments preservation */
static int test_comments_preservation() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "mov rax, rax  ; This is a comment\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "This is a comment") != NULL, "Comment was not preserved");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_comments_preservation");
}

/* Test with directives and labels */
static int test_directives_and_labels() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = ".text\n.globl main\nmain:\nmov rax, 0\nret\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, ".text") != NULL, "Directive was removed");
    TEST_ASSERT(strstr(output, ".globl main") != NULL, "Directive was removed");
    TEST_ASSERT(strstr(output, "main:") != NULL, "Label was removed");
    TEST_ASSERT(strstr(output, "xor rax, rax") != NULL, "Optimization not applied");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_directives_and_labels");
}

/* Test Pattern 10: add 1 to inc */
static int test_add_one_to_inc() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "add rax, 1\nadd rbx, 2\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "inc rax") != NULL, "add 1 not converted to inc");
    TEST_ASSERT(strstr(output, "add rbx, 2") != NULL, "add 2 was incorrectly changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_add_one_to_inc");
}

/* Test Pattern 12: redundant swap move elimination */
static int test_swap_move_elimination() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "mov rax, rbx\nmov rbx, rax\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "mov rax, rbx") != NULL, "First mov should remain");
    TEST_ASSERT(strstr(output, "mov rbx, rax") == NULL, "Second mov not removed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_swap_move_elimination");
}

/* Test Pattern 13: sub reg, reg -> xor reg, reg */
static int test_sub_self_to_xor() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "sub rax, rax\nsub rbx, rcx\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "xor rax, rax") != NULL, "sub self not converted to xor");
    TEST_ASSERT(strstr(output, "sub rbx, rcx") != NULL, "Non-self sub was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_sub_self_to_xor");
}

/* Test Pattern 14: and reg, 0 -> xor reg, reg */
static int test_and_zero_to_xor() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "and rax, 0\nand rbx, 5\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "xor rax, rax") != NULL, "and 0 not converted to xor");
    TEST_ASSERT(strstr(output, "and rbx, 5") != NULL, "and with non-zero was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_and_zero_to_xor");
}

/* Test Pattern 15: cmp reg, 0 -> test reg, reg */
static int test_cmp_zero_to_test() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "cmp rax, 0\ncmp rbx, 7\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "test rax, rax") != NULL, "cmp 0 not converted to test");
    TEST_ASSERT(strstr(output, "cmp rbx, 7") != NULL, "Non-zero cmp was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_cmp_zero_to_test");
}

/* Test Pattern 16: or reg, reg -> test reg, reg */
static int test_or_self_to_test() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "or rax, rax\nor rbx, rcx\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "test rax, rax") != NULL, "or self not converted to test");
    TEST_ASSERT(strstr(output, "or rbx, rcx") != NULL, "Non-self or was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_or_self_to_test");
}

/* Test Pattern 17: add reg, -1 -> dec reg */
static int test_add_minus_one_to_dec() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "add rax, -1\nadd rbx, 2\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "dec rax") != NULL, "add -1 not converted to dec");
    TEST_ASSERT(strstr(output, "add rbx, 2") != NULL, "Non -1 add was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_add_minus_one_to_dec");
}

/* Test Pattern 18: sub reg, -1 -> inc reg */
static int test_sub_minus_one_to_inc() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "sub rax, -1\nsub rbx, 3\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "inc rax") != NULL, "sub -1 not converted to inc");
    TEST_ASSERT(strstr(output, "sub rbx, 3") != NULL, "Non -1 sub was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_sub_minus_one_to_inc");
}

/* Test Pattern 19: and reg, reg -> test reg, reg */
static int test_and_self_to_test() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "and rax, rax\nand rbx, rcx\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "test rax, rax") != NULL, "and self not converted to test");
    TEST_ASSERT(strstr(output, "and rbx, rcx") != NULL, "Non-self and was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_and_self_to_test");
}

/* Test Pattern 20: cmp reg, reg -> test reg, reg */
static int test_cmp_self_to_test() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "cmp rax, rax\ncmp rbx, rcx\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "test rax, rax") != NULL, "cmp self not converted to test");
    TEST_ASSERT(strstr(output, "cmp rbx, rcx") != NULL, "Non-self cmp was changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_cmp_self_to_test");
}

/* Test Pattern 21: fallthrough jump removal */
static int test_fallthrough_jump_removal() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "jmp .next\n.next:\nmov rax, 0\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "jmp .next") == NULL, "Fallthrough jump not removed");
    TEST_ASSERT(strstr(output, ".next:") != NULL, "Label removed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_fallthrough_jump_removal");
}

/* Test Pattern 22: hot loop alignment */
static int test_hot_loop_alignment() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    asmopt_set_option(ctx, "hot_align", "1");
    const char* input = ".hot_loop:\nadd rax, 1\n";
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

/* Test Pattern 23: bsf to tzcnt on Zen */
static int test_bsf_to_tzcnt() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    asmopt_set_target_cpu(ctx, "zen3");
    
    const char* input = "bsf rax, rbx\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "tzcnt rax, rbx") != NULL, "bsf not converted to tzcnt");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_bsf_to_tzcnt");
}

/* Test Pattern 24 removed: bsr -> lzcnt not applied */

/* Test Pattern 11: sub 1 to dec */
static int test_sub_one_to_dec() {
    asmopt_context* ctx = asmopt_create("x86-64");
    TEST_ASSERT(ctx != NULL, "Failed to create context");
    
    const char* input = "sub rax, 1\nsub rbx, 3\n";
    asmopt_parse_string(ctx, input);
    asmopt_optimize(ctx);
    
    char* output = asmopt_generate_assembly(ctx);
    TEST_ASSERT(output != NULL, "Failed to generate output");
    TEST_ASSERT(strstr(output, "dec rax") != NULL, "sub 1 not converted to dec");
    TEST_ASSERT(strstr(output, "sub rbx, 3") != NULL, "sub 3 was incorrectly changed");
    
    free(output);
    asmopt_destroy(ctx);
    TEST_PASS("test_sub_one_to_dec");
}

int main() {
    int passed = 0;
    int total = 0;
    
    printf("Running asmopt unit tests...\n\n");
    
    total++; passed += test_redundant_mov();
    total++; passed += test_mov_zero_to_xor();
    total++; passed += test_mul_by_one();
    total++; passed += test_mul_power_of_2();
    total++; passed += test_add_sub_zero();
    total++; passed += test_shift_zero();
    total++; passed += test_or_zero();
    total++; passed += test_xor_zero();
    total++; passed += test_add_one_to_inc();
    total++; passed += test_sub_one_to_dec();
    total++; passed += test_swap_move_elimination();
    total++; passed += test_sub_self_to_xor();
    total++; passed += test_and_zero_to_xor();
    total++; passed += test_cmp_zero_to_test();
    total++; passed += test_or_self_to_test();
    total++; passed += test_add_minus_one_to_dec();
    total++; passed += test_sub_minus_one_to_inc();
    total++; passed += test_and_self_to_test();
    total++; passed += test_cmp_self_to_test();
    total++; passed += test_fallthrough_jump_removal();
    total++; passed += test_hot_loop_alignment();
    total++; passed += test_bsf_to_tzcnt();
    total++; passed += test_optimization_stats();
    total++; passed += test_report_generation();
    total++; passed += test_context_lifecycle();
    total++; passed += test_comments_preservation();
    total++; passed += test_directives_and_labels();
    
    printf("\n========================================\n");
    printf("Test Results: %d/%d tests passed\n", passed, total);
    printf("========================================\n");
    
    return (passed == total) ? 0 : 1;
}
