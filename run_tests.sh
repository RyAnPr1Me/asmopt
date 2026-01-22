#!/bin/bash

# Test runner script for asmopt
# Runs all tests and generates coverage report

set -e

echo "======================================"
echo "asmopt Test Suite"
echo "======================================"
echo ""

# Build the project
echo "Building project..."
cmake -S . -B build
cmake --build build
echo ""

# Run tests
echo "Running tests..."
cd build
ctest --output-on-failure --verbose
cd ..
echo ""

# Count test results
echo "======================================"
echo "Test Summary:"
echo "======================================"
cd build
./test_peephole 2>&1 | grep "Test Results"
./test_integration 2>&1 | grep "Test Results"
cd ..

echo ""
echo "All tests completed successfully!"
