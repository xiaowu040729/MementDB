#!/bin/bash

# MementoDB 停止脚本

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PID_FILE="./mementodb.pid"

# 检查 PID 文件是否存在
if [ ! -f "$PID_FILE" ]; then
    echo -e "${YELLOW}未找到 PID 文件，服务器可能未运行${NC}"
    exit 0
fi

PID=$(cat "$PID_FILE")

# 检查进程是否存在
if ! ps -p "$PID" > /dev/null 2>&1; then
    echo -e "${YELLOW}进程不存在 (PID: $PID)，删除过期的 PID 文件${NC}"
    rm -f "$PID_FILE"
    exit 0
fi

# 停止服务器
echo -e "${BLUE}正在停止 MementoDB 服务器 (PID: $PID)...${NC}"

# 尝试优雅停止
kill "$PID" 2>/dev/null || true

# 等待进程结束
for i in {1..10}; do
    if ! ps -p "$PID" > /dev/null 2>&1; then
        break
    fi
    sleep 1
done

# 如果还在运行，强制停止
if ps -p "$PID" > /dev/null 2>&1; then
    echo -e "${YELLOW}进程未响应，强制停止...${NC}"
    kill -9 "$PID" 2>/dev/null || true
    sleep 1
fi

# 清理 PID 文件
rm -f "$PID_FILE"

if ps -p "$PID" > /dev/null 2>&1; then
    echo -e "${RED}错误: 无法停止服务器${NC}"
    exit 1
else
    echo -e "${GREEN}✓ 服务器已停止${NC}"
    exit 0
fi
