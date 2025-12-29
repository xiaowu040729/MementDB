#!/usr/bin/env bash

set -euo pipefail

# Resolve project root (directory of this script)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

mkdir -p "${BUILD_DIR}"

cd "${BUILD_DIR}"

# 如果之前在 Windows/Visual Studio 下生成过缓存，这里强制清理掉，避免生成器不兼容
if [ -f "CMakeCache.txt" ]; then
  echo "[MementoDB] Detected existing CMake cache, removing old cache for a clean configure..."
  rm -f CMakeCache.txt
  rm -rf CMakeFiles
fi

echo "[MementoDB] Configuring with CMake (Unix generator)..."
cmake ..

echo "[MementoDB] Building..."
cmake --build .

# 回到项目根目录再运行，这样日志路径就是 <项目根>/logs/main_logging.log
cd "${SCRIPT_DIR}"

echo "[MementoDB] Running executable..."
if [ -x "${BUILD_DIR}/Debug/outDebug" ]; then
  "${BUILD_DIR}/Debug/outDebug"
elif [ -x "${BUILD_DIR}/outDebug" ]; then
  # Fallback in case output directory layout is different
  "${BUILD_DIR}/outDebug"
else
  echo "Error: Could not find built executable 'outDebug'." >&2
  exit 1
fi


