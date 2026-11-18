#!/usr/bin/env bash
#
# Build and run the dr_wav 64-bit memory-writer acceptance tests.
#
# No third-party libraries and no network access are required: everything is built from the
# single dr_wav.h in the repository root. Output artifacts go into tests/memory_write/build.
#
# Usage: ./run_tests.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

#
# The sparse >4GB case briefly allocates real pages across a 4 GiB region before the hole is punched,
# so its working directory must sit on a filesystem with enough free space (and sparse-file support).
# Default to a directory next to the script on the repository's volume; override with MEMWRITE_TEST_DIR.
#
TEST_WORK_DIR="${MEMWRITE_TEST_DIR:-${BUILD_DIR}/work}"

mkdir -p "${BUILD_DIR}" "${TEST_WORK_DIR}"

WARNINGS="-Wall -Wextra -Wpedantic -Wdeclaration-after-statement"

echo "== Compiling strict C99 build =="
gcc -std=c99 ${WARNINGS} -Werror -O2 \
    -I"${REPO_ROOT}" \
    "${SCRIPT_DIR}/dr_wav_memory_write_test.c" \
    -o "${BUILD_DIR}/dr_wav_memory_write_test_c99"

echo "== Compiling C++ build (header must compile as C++ too) =="
g++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -O2 \
    -I"${REPO_ROOT}" \
    "${SCRIPT_DIR}/dr_wav_memory_write_test.c" \
    -o "${BUILD_DIR}/dr_wav_memory_write_test_cpp"

echo "== Compiling ASan/UBSan debug build =="
gcc -std=c99 ${WARNINGS} -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"${REPO_ROOT}" \
    "${SCRIPT_DIR}/dr_wav_memory_write_test.c" \
    -o "${BUILD_DIR}/dr_wav_memory_write_test_asan"

echo
echo "== Running C99 test binary =="
"${BUILD_DIR}/dr_wav_memory_write_test_c99" "${TEST_WORK_DIR}"

echo
echo "== Running C++ test binary =="
"${BUILD_DIR}/dr_wav_memory_write_test_cpp" "${TEST_WORK_DIR}"

echo
echo "== Running ASan/UBSan test binary =="
"${BUILD_DIR}/dr_wav_memory_write_test_asan" "${TEST_WORK_DIR}"

echo
echo "All builds and test runs succeeded."
