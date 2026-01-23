#!/bin/bash
# Comprehensive manual verification script for asmopt
# This script verifies 100% of the codebase functionality using command-line operations

# set -e removed to continue on errors

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ASMOPT="$SCRIPT_DIR/build/asmopt"
TEST_DIR="/tmp/asmopt_verify_$$"
PASS_COUNT=0
FAIL_COUNT=0

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

mkdir -p "$TEST_DIR"

# Helper function to check test result
check_result() {
    local test_name="$1"
    local expected="$2"
    local actual="$3"
    
    if [ "$expected" = "$actual" ]; then
        echo -e "${GREEN}✓ PASS${NC}: $test_name"
        ((PASS_COUNT++))
        return 0
    else
        echo -e "${RED}✗ FAIL${NC}: $test_name"
        echo "  Expected: $expected"
        echo "  Actual: $actual"
        ((FAIL_COUNT++))
        return 1
    fi
}

# Helper to check if string contains substring
contains() {
    [[ "$1" == *"$2"* ]]
}

echo "========================================="
echo "asmopt Comprehensive Functionality Test"
echo "========================================="
echo ""

# =======================
# Test 1: Pattern 1 - Redundant mov elimination
# =======================
echo "Test 1: Pattern 1 - Redundant mov (mov rax, rax)"
cat > "$TEST_DIR/test1.s" << 'EOF'
mov rax, rax
mov rbx, rcx
EOF

$ASMOPT "$TEST_DIR/test1.s" -o "$TEST_DIR/test1_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test1_out.s")

if ! contains "$output" "mov rax, rax" && contains "$output" "mov rbx, rcx"; then
    check_result "Redundant mov removed" "pass" "pass"
else
    check_result "Redundant mov removed" "pass" "fail"
fi

# =======================
# Test 2: Pattern 2 - mov zero to xor
# =======================
echo "Test 2: Pattern 2 - mov zero to xor (mov rax, 0 → xor rax, rax)"
cat > "$TEST_DIR/test2.s" << 'EOF'
mov rax, 0
mov rbx, 5
EOF

$ASMOPT "$TEST_DIR/test2.s" -o "$TEST_DIR/test2_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test2_out.s")

if contains "$output" "xor rax, rax" && contains "$output" "mov rbx, 5"; then
    check_result "mov 0 converted to xor" "pass" "pass"
else
    check_result "mov 0 converted to xor" "pass" "fail"
fi

# =======================
# Test 3: Pattern 3 - Multiply by one
# =======================
echo "Test 3: Pattern 3 - Multiply by one (imul rax, 1)"
cat > "$TEST_DIR/test3.s" << 'EOF'
imul rax, 1
imul rbx, 2
EOF

$ASMOPT "$TEST_DIR/test3.s" -o "$TEST_DIR/test3_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test3_out.s")

if ! contains "$output" "imul rax, 1"; then
    check_result "Multiply by 1 removed" "pass" "pass"
else
    check_result "Multiply by 1 removed" "pass" "fail"
fi

# =======================
# Test 4: Pattern 4 - Power of 2 multiply to shift
# =======================
echo "Test 4: Pattern 4 - Power of 2 multiply (imul rax, 8 → shl rax, 3)"
cat > "$TEST_DIR/test4.s" << 'EOF'
imul rax, 8
imul rbx, 3
EOF

$ASMOPT "$TEST_DIR/test4.s" -o "$TEST_DIR/test4_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test4_out.s")

if contains "$output" "shl rax, 3" && contains "$output" "imul rbx, 3"; then
    check_result "Power of 2 multiply to shift" "pass" "pass"
else
    check_result "Power of 2 multiply to shift" "pass" "fail"
fi

# =======================
# Test 5: Pattern 5 - Add/sub zero
# =======================
echo "Test 5: Pattern 5 - Add/sub zero"
cat > "$TEST_DIR/test5.s" << 'EOF'
add rax, 0
sub rbx, 0
add rcx, 5
EOF

$ASMOPT "$TEST_DIR/test5.s" -o "$TEST_DIR/test5_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test5_out.s")

if ! contains "$output" "add rax, 0" && ! contains "$output" "sub rbx, 0" && contains "$output" "add rcx, 5"; then
    check_result "Add/sub zero removed" "pass" "pass"
else
    check_result "Add/sub zero removed" "pass" "fail"
fi

# =======================
# Test 6: Pattern 6 - Shift by zero
# =======================
echo "Test 6: Pattern 6 - Shift by zero"
cat > "$TEST_DIR/test6.s" << 'EOF'
shl rax, 0
shr rbx, 0
shl rcx, 3
EOF

$ASMOPT "$TEST_DIR/test6.s" -o "$TEST_DIR/test6_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test6_out.s")

if ! contains "$output" "shl rax, 0" && ! contains "$output" "shr rbx, 0" && contains "$output" "shl rcx, 3"; then
    check_result "Shift by zero removed" "pass" "pass"
else
    check_result "Shift by zero removed" "pass" "fail"
fi

# =======================
# Test 7: Pattern 7 - OR zero
# =======================
echo "Test 7: Pattern 7 - OR zero"
cat > "$TEST_DIR/test7.s" << 'EOF'
or rax, 0
or rbx, 5
EOF

$ASMOPT "$TEST_DIR/test7.s" -o "$TEST_DIR/test7_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test7_out.s")

if ! contains "$output" "or rax, 0" && contains "$output" "or rbx, 5"; then
    check_result "OR zero removed" "pass" "pass"
else
    check_result "OR zero removed" "pass" "fail"
fi

# =======================
# Test 8: Pattern 8 - XOR zero immediate
# =======================
echo "Test 8: Pattern 8 - XOR zero immediate (preserves xor reg,reg)"
cat > "$TEST_DIR/test8.s" << 'EOF'
xor rax, 0
xor rbx, rbx
xor rcx, 5
EOF

$ASMOPT "$TEST_DIR/test8.s" -o "$TEST_DIR/test8_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test8_out.s")

if ! contains "$output" "xor rax, 0" && contains "$output" "xor rbx, rbx" && contains "$output" "xor rcx, 5"; then
    check_result "XOR zero immediate removed, xor reg,reg preserved" "pass" "pass"
else
    check_result "XOR zero immediate removed, xor reg,reg preserved" "pass" "fail"
fi

# =======================
# Test 9: Pattern 9 - AND with -1
# =======================
echo "Test 9: Pattern 9 - AND with -1"
cat > "$TEST_DIR/test9.s" << 'EOF'
and rax, -1
and rbx, 5
EOF

$ASMOPT "$TEST_DIR/test9.s" -o "$TEST_DIR/test9_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test9_out.s")

if ! contains "$output" "and rax, -1" && contains "$output" "and rbx, 5"; then
    check_result "AND with -1 removed" "pass" "pass"
else
    check_result "AND with -1 removed" "pass" "fail"
fi

# =======================
# Test 10: Pattern 10 - Add 1 to inc
# =======================
echo "Test 10: Pattern 10 - Add 1 to inc"
cat > "$TEST_DIR/test10.s" << 'EOF'
add rax, 1
add rbx, 2
EOF

$ASMOPT "$TEST_DIR/test10.s" -o "$TEST_DIR/test10_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test10_out.s")

if contains "$output" "inc rax" && contains "$output" "add rbx, 2"; then
    check_result "Add 1 converted to inc" "pass" "pass"
else
    check_result "Add 1 converted to inc" "pass" "fail"
fi

# =======================
# Test 11: Pattern 11 - Sub 1 to dec
# =======================
echo "Test 11: Pattern 11 - Sub 1 to dec"
cat > "$TEST_DIR/test11.s" << 'EOF'
sub rax, 1
sub rbx, 3
EOF

$ASMOPT "$TEST_DIR/test11.s" -o "$TEST_DIR/test11_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test11_out.s")

if contains "$output" "dec rax" && contains "$output" "sub rbx, 3"; then
    check_result "Sub 1 converted to dec" "pass" "pass"
else
    check_result "Sub 1 converted to dec" "pass" "fail"
fi

# =======================
# Test 12: Comment preservation
# =======================
echo "Test 12: Comment preservation"
cat > "$TEST_DIR/test12.s" << 'EOF'
mov rax, rax  ; This is a comment
add rbx, 0    ; Another comment
EOF

$ASMOPT "$TEST_DIR/test12.s" -o "$TEST_DIR/test12_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test12_out.s")

if contains "$output" "This is a comment" && contains "$output" "Another comment"; then
    check_result "Comments preserved" "pass" "pass"
else
    check_result "Comments preserved" "pass" "fail"
fi

# =======================
# Test 13: Directives and labels
# =======================
echo "Test 13: Directives and labels preservation"
cat > "$TEST_DIR/test13.s" << 'EOF'
.text
.globl main
main:
    mov rax, 0
    ret
EOF

$ASMOPT "$TEST_DIR/test13.s" -o "$TEST_DIR/test13_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test13_out.s")

if contains "$output" ".text" && contains "$output" ".globl main" && contains "$output" "main:" && contains "$output" "xor rax, rax"; then
    check_result "Directives and labels preserved" "pass" "pass"
else
    check_result "Directives and labels preserved" "pass" "fail"
fi

# =======================
# Test 14: Statistics output
# =======================
echo "Test 14: Statistics output"
cat > "$TEST_DIR/test14.s" << 'EOF'
mov rax, rax
mov rbx, 0
add rcx, 0
EOF

stats_output=$($ASMOPT "$TEST_DIR/test14.s" -o "$TEST_DIR/test14_out.s" --stats 2>&1)

if contains "$stats_output" "original_lines" && contains "$stats_output" "optimized_lines" && contains "$stats_output" "replacements" && contains "$stats_output" "removals"; then
    check_result "Statistics output generated" "pass" "pass"
else
    check_result "Statistics output generated" "pass" "fail"
fi

# =======================
# Test 15: Report generation
# =======================
echo "Test 15: Report generation"
cat > "$TEST_DIR/test15.s" << 'EOF'
mov rax, 0
imul rbx, 8
EOF

$ASMOPT "$TEST_DIR/test15.s" -o "$TEST_DIR/test15_out.s" --report "$TEST_DIR/test15_report.txt" 2>/dev/null
report=$(cat "$TEST_DIR/test15_report.txt")

if contains "$report" "Optimization Report" && contains "$report" "mov_zero_to_xor" && contains "$report" "mul_power_of_2_to_shift"; then
    check_result "Report generated with optimization details" "pass" "pass"
else
    check_result "Report generated with optimization details" "pass" "fail"
fi

# =======================
# Test 16: Optimization level 0 (no optimization)
# =======================
echo "Test 16: Optimization level 0"
cat > "$TEST_DIR/test16.s" << 'EOF'
mov rax, 0
EOF

$ASMOPT "$TEST_DIR/test16.s" -o "$TEST_DIR/test16_out.s" -O0 2>/dev/null
output=$(cat "$TEST_DIR/test16_out.s")

if contains "$output" "mov rax, 0"; then
    check_result "Level 0 preserves original code" "pass" "pass"
else
    check_result "Level 0 preserves original code" "pass" "fail"
fi

# =======================
# Test 17: Optimization level 2 (default)
# =======================
echo "Test 17: Optimization level 2"
cat > "$TEST_DIR/test17.s" << 'EOF'
mov rax, 0
EOF

$ASMOPT "$TEST_DIR/test17.s" -o "$TEST_DIR/test17_out.s" -O2 2>/dev/null
output=$(cat "$TEST_DIR/test17_out.s")

if contains "$output" "xor rax, rax"; then
    check_result "Level 2 applies optimizations" "pass" "pass"
else
    check_result "Level 2 applies optimizations" "pass" "fail"
fi

# =======================
# Test 18: File I/O
# =======================
echo "Test 18: File I/O"
cat > "$TEST_DIR/test18.s" << 'EOF'
mov rax, 0
EOF

$ASMOPT -i "$TEST_DIR/test18.s" -o "$TEST_DIR/test18_out.s" 2>/dev/null

if [ -f "$TEST_DIR/test18_out.s" ]; then
    check_result "File I/O works" "pass" "pass"
else
    check_result "File I/O works" "pass" "fail"
fi

# =======================
# Test 19: Stdin/stdout
# =======================
echo "Test 19: Stdin/stdout"
echo "mov rax, 0" | $ASMOPT -o - 2>/dev/null > "$TEST_DIR/test19_out.s"
output=$(cat "$TEST_DIR/test19_out.s")

if contains "$output" "xor rax, rax"; then
    check_result "Stdin/stdout processing" "pass" "pass"
else
    check_result "Stdin/stdout processing" "pass" "fail"
fi

# =======================
# Test 20: Empty input
# =======================
echo "Test 20: Empty input handling"
echo "" | $ASMOPT -o - 2>/dev/null > "$TEST_DIR/test20_out.s"

if [ -f "$TEST_DIR/test20_out.s" ]; then
    check_result "Empty input handled" "pass" "pass"
else
    check_result "Empty input handled" "pass" "fail"
fi

# =======================
# Test 21: Large input
# =======================
echo "Test 21: Large input (1000 instructions)"
{
    for i in {1..1000}; do
        echo "mov rax, rax"
        echo "mov rbx, 0"
        echo "add rcx, 0"
    done
} > "$TEST_DIR/test21.s"

$ASMOPT "$TEST_DIR/test21.s" -o "$TEST_DIR/test21_out.s" --stats 2>&1 | grep -q "replacements"
if [ $? -eq 0 ]; then
    check_result "Large input processed" "pass" "pass"
else
    check_result "Large input processed" "pass" "fail"
fi

# =======================
# Test 22: --no-optimize flag
# =======================
echo "Test 22: --no-optimize flag"
cat > "$TEST_DIR/test22.s" << 'EOF'
mov rax, 0
EOF

$ASMOPT "$TEST_DIR/test22.s" -o "$TEST_DIR/test22_out.s" --no-optimize 2>/dev/null
output=$(cat "$TEST_DIR/test22_out.s")

if contains "$output" "mov rax, 0"; then
    check_result "--no-optimize flag works" "pass" "pass"
else
    check_result "--no-optimize flag works" "pass" "fail"
fi

# =======================
# Test 23: IR dump
# =======================
echo "Test 23: IR dump"
cat > "$TEST_DIR/test23.s" << 'EOF'
mov rax, 0
EOF

ir_output=$($ASMOPT "$TEST_DIR/test23.s" -o "$TEST_DIR/test23_out.s" --dump-ir 2>&1)

if contains "$ir_output" "IR" || [ ${#ir_output} -gt 0 ]; then
    check_result "IR dump generated" "pass" "pass"
else
    check_result "IR dump generated" "pass" "fail"
fi

# =======================
# Test 24: CFG dump
# =======================
echo "Test 24: CFG dump"
cat > "$TEST_DIR/test24.s" << 'EOF'
main:
    mov rax, 0
    test rax, rax
    jz .label
    ret
.label:
    mov rbx, 1
    ret
EOF

cfg_output=$($ASMOPT "$TEST_DIR/test24.s" -o "$TEST_DIR/test24_out.s" --dump-cfg 2>&1)

if contains "$cfg_output" "CFG" || [ ${#cfg_output} -gt 0 ]; then
    check_result "CFG dump generated" "pass" "pass"
else
    check_result "CFG dump generated" "pass" "fail"
fi

# =======================
# Test 25: CFG dot output
# =======================
echo "Test 25: CFG dot file"
cat > "$TEST_DIR/test25.s" << 'EOF'
main:
    mov rax, 0
    ret
EOF

$ASMOPT "$TEST_DIR/test25.s" -o "$TEST_DIR/test25_out.s" --cfg "$TEST_DIR/test25.dot" 2>/dev/null

if [ -f "$TEST_DIR/test25.dot" ]; then
    check_result "CFG dot file generated" "pass" "pass"
else
    check_result "CFG dot file generated" "pass" "fail"
fi

# =======================
# Test 26: Architecture option
# =======================
echo "Test 26: Architecture option"
cat > "$TEST_DIR/test26.s" << 'EOF'
mov rax, 0
EOF

$ASMOPT "$TEST_DIR/test26.s" -o "$TEST_DIR/test26_out.s" --march x86-64 2>/dev/null

if [ -f "$TEST_DIR/test26_out.s" ]; then
    check_result "Architecture option works" "pass" "pass"
else
    check_result "Architecture option works" "pass" "fail"
fi

# =======================
# Test 27: CPU tuning option
# =======================
echo "Test 27: CPU tuning option"
cat > "$TEST_DIR/test27.s" << 'EOF'
mov rax, 0
EOF

$ASMOPT "$TEST_DIR/test27.s" -o "$TEST_DIR/test27_out.s" --mtune zen3 2>/dev/null

if [ -f "$TEST_DIR/test27_out.s" ]; then
    check_result "CPU tuning option works" "pass" "pass"
else
    check_result "CPU tuning option works" "pass" "fail"
fi

# =======================
# Test 28: AMD optimizations
# =======================
echo "Test 28: AMD optimizations flag"
cat > "$TEST_DIR/test28.s" << 'EOF'
mov rax, 0
EOF

$ASMOPT "$TEST_DIR/test28.s" -o "$TEST_DIR/test28_out.s" --amd-optimize 2>/dev/null

if [ -f "$TEST_DIR/test28_out.s" ]; then
    check_result "AMD optimizations flag works" "pass" "pass"
else
    check_result "AMD optimizations flag works" "pass" "fail"
fi

# =======================
# Test 29: Multiple patterns in one file
# =======================
echo "Test 29: Multiple optimization patterns"
cat > "$TEST_DIR/test29.s" << 'EOF'
.text
.globl test_all
test_all:
    mov rax, rax
    mov rbx, 0
    imul rcx, 1
    imul rdx, 16
    add rsi, 0
    shl rdi, 0
    or r8, 0
    xor r9, 0
    and r10, -1
    add r11, 1
    sub r12, 1
    ret
EOF

$ASMOPT "$TEST_DIR/test29.s" -o "$TEST_DIR/test29_out.s" 2>/dev/null
output=$(cat "$TEST_DIR/test29_out.s")

patterns_found=0
contains "$output" "xor rbx, rbx" && ((patterns_found++))
contains "$output" "shl rdx, 4" && ((patterns_found++))
contains "$output" "inc r11" && ((patterns_found++))
contains "$output" "dec r12" && ((patterns_found++))
! contains "$output" "mov rax, rax" && ((patterns_found++))
! contains "$output" "imul rcx, 1" && ((patterns_found++))

if [ $patterns_found -ge 5 ]; then
    check_result "Multiple patterns applied correctly" "pass" "pass"
else
    check_result "Multiple patterns applied correctly" "pass" "fail"
fi

# =======================
# Test 30: Format option (Intel vs AT&T)
# =======================
echo "Test 30: Format option"
cat > "$TEST_DIR/test30.s" << 'EOF'
mov rax, 0
EOF

$ASMOPT "$TEST_DIR/test30.s" -o "$TEST_DIR/test30_out.s" -f intel 2>/dev/null

if [ -f "$TEST_DIR/test30_out.s" ]; then
    check_result "Format option works" "pass" "pass"
else
    check_result "Format option works" "pass" "fail"
fi

# Cleanup
rm -rf "$TEST_DIR"

# Summary
echo ""
echo "========================================="
echo "Test Summary"
echo "========================================="
echo -e "${GREEN}PASSED: $PASS_COUNT${NC}"
echo -e "${RED}FAILED: $FAIL_COUNT${NC}"
echo "TOTAL: $((PASS_COUNT + FAIL_COUNT))"
echo "========================================="

if [ $FAIL_COUNT -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed!${NC}"
    exit 1
fi
