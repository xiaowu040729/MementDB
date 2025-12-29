#!/bin/bash
# 快速运行功能测试的脚本

cd "$(dirname "$0")/build"
if [ -f src/core/test_core_functional ]; then
    ./src/core/test_core_functional
else
    echo "错误: 测试程序不存在，请先运行: ./test_core.sh --functional"
    exit 1
fi
