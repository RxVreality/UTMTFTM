#!/bin/sh
# Configures, builds and runs the offline tests on ARM64 Linux. Finished program: dist/
set -e
cd "$(dirname "$0")"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
[ "${1:-}" = notest ] && exit 0
dist/test_ghost
dist/test_session
dist/test_driver
dist/test_controllers
mkdir -p build/ui
dist/test_ui build/ui
dist/test_bindings build/ui
