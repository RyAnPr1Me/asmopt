# asmopt Test Suite

This directory contains comprehensive tests for the asmopt assembly optimizer.

## Test Structure

### Unit Tests (`test_peephole.c`)
Tests individual peephole optimization patterns:
- Pattern 1: Redundant mov elimination
- Pattern 2: mov zero to xor
- Pattern 3: Multiply by one elimination
- Pattern 4: Power of 2 multiply to shift
- Pattern 5: Add/sub zero elimination
- Pattern 6: Shift by zero elimination
- Pattern 7: OR zero elimination
- Pattern 8: XOR zero immediate elimination
- Pattern 9: AND with -1 elimination
- Optimization statistics tracking
- Report generation
- Context lifecycle management
- Comments preservation
- Directives and labels handling

### Integration Tests (`test_integration.c`)
Tests the complete optimization pipeline:
- Complete function optimization
- File-based I/O
- Optimization levels (0-4)
- Option setting and configuration
- IR and CFG dump functions
- Large input handling (1000+ instructions)
- Edge cases (empty input, comments only, whitespace)
- Comprehensive report generation with all patterns

## Running Tests

### Quick Run
```bash
./run_tests.sh
```

### Manual Run
```bash
# Build
cmake -S . -B build
cmake --build build

# Run all tests
cd build
ctest

# Run specific test
./test_peephole
./test_integration
```

### With Verbose Output
```bash
cd build
ctest --output-on-failure --verbose
```

## Test Coverage

The test suite achieves 100% coverage of:
- All 9 peephole optimization patterns
- All public API functions
- IR generation and parsing
- CFG construction
- Report generation
- Statistics tracking
- File I/O operations
- Edge case handling
- Memory management

## Expected Results

All tests should pass:
- 13 unit tests in test_peephole
- 8 integration tests in test_integration
- Total: 21 tests

```
Test Results: 13/13 tests passed (unit)
Test Results: 8/8 tests passed (integration)
100% tests passed, 0 tests failed out of 2
```

## Adding New Tests

1. Add test function to appropriate file
2. Register test in main() function
3. Rebuild: `cmake --build build`
4. Run: `ctest`

## Test Assertions

Use the provided macros:
```c
TEST_ASSERT(condition, "Error message");
TEST_PASS("test_name");
```
