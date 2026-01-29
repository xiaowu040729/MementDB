#!/bin/bash

# MementoDB 测试脚本
# 用于测试数据库的基本功能

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

HOST="127.0.0.1"
PORT="6380"

echo -e "${BLUE}=== MementoDB 测试脚本 ===${NC}\n"

# 检查 redis-cli 是否可用
if ! command -v redis-cli &> /dev/null; then
    echo -e "${YELLOW}警告: redis-cli 未找到，将使用 telnet 进行测试${NC}"
    USE_TELNET=true
else
    USE_TELNET=false
fi

# 测试函数
test_command() {
    local cmd="$1"
    local expected="$2"
    local description="$3"
    
    echo -e "${BLUE}测试: ${description}${NC}"
    echo -e "  命令: ${YELLOW}${cmd}${NC}"
    
    if [ "$USE_TELNET" = false ]; then
        result=$(redis-cli -h "$HOST" -p "$PORT" $cmd 2>&1)
    else
        # 使用 telnet（需要手动处理）
        echo -e "${YELLOW}  跳过（需要 redis-cli）${NC}"
        return
    fi
    
    echo -e "  结果: ${GREEN}${result}${NC}"
    
    if [ -n "$expected" ] && [ "$result" != "$expected" ]; then
        echo -e "  ${RED}✗ 期望: ${expected}${NC}"
    else
        echo -e "  ${GREEN}✓ 通过${NC}"
    fi
    echo ""
}

# 检查服务器是否运行
echo -e "${BLUE}检查服务器状态...${NC}"
if [ "$USE_TELNET" = false ]; then
    if redis-cli -h "$HOST" -p "$PORT" PING &>/dev/null; then
        echo -e "${GREEN}✓ 服务器正在运行${NC}\n"
    else
        echo -e "${RED}✗ 无法连接到服务器${NC}"
        echo -e "${YELLOW}请先启动服务器: ./start.sh${NC}"
        exit 1
    fi
else
    echo -e "${YELLOW}跳过连接检查（需要 redis-cli）${NC}\n"
fi

# 运行测试
echo -e "${BLUE}开始测试...${NC}\n"

# 1. PING 测试
test_command "PING" "PONG" "PING 命令"

# 2. SET 测试
test_command "SET test_key 'Hello MementoDB'" "OK" "SET 命令"

# 3. GET 测试
test_command "GET test_key" "Hello MementoDB" "GET 命令"

# 4. EXISTS 测试
test_command "EXISTS test_key" "1" "EXISTS 命令（键存在）"
test_command "EXISTS nonexistent_key" "0" "EXISTS 命令（键不存在）"

# 5. SET 多个键
test_command "SET user:1:name 'Alice'" "OK" "SET 用户数据"
test_command "SET user:1:email 'alice@example.com'" "OK" "SET 用户邮箱"
test_command "SET user:1:age '30'" "OK" "SET 用户年龄"

# 6. GET 多个键
test_command "GET user:1:name" "Alice" "GET 用户名称"
test_command "GET user:1:email" "alice@example.com" "GET 用户邮箱"
test_command "GET user:1:age" "30" "GET 用户年龄"

# 7. DEL 测试
test_command "DEL test_key" "1" "DEL 命令"
test_command "GET test_key" "" "GET 已删除的键（应返回空）"

# 8. INFO 测试
test_command "INFO" "" "INFO 命令"

echo -e "${GREEN}=== 测试完成 ===${NC}\n"

# 交互式模式
echo -e "${BLUE}进入交互式模式（输入 'exit' 退出）...${NC}\n"

if [ "$USE_TELNET" = false ]; then
    redis-cli -h "$HOST" -p "$PORT"
else
    echo -e "${YELLOW}交互式模式需要 redis-cli${NC}"
    echo -e "${BLUE}可以使用以下命令手动测试:${NC}"
    echo -e "  ${YELLOW}telnet ${HOST} ${PORT}${NC}"
    echo -e "  然后输入 Redis 命令，例如:"
    echo -e "  ${YELLOW}SET mykey myvalue${NC}"
    echo -e "  ${YELLOW}GET mykey${NC}"
fi
