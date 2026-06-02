#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${ASHIATO_LINT_BUILD_DIR:-${project_dir}/build-lint}"

cmake -S "${project_dir}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DASHIATO_BUILD_EXAMPLE=OFF \
    -DASHIATO_BUILD_TESTING=OFF \
    -DASHIATO_BUILD_BENCHMARKS=OFF \
    -DASHIATO_ENABLE_CLANG_TIDY=ON \
    -DASHIATO_ENABLE_CPPCHECK=ON

cmake --build "${build_dir}" --target ashiato
