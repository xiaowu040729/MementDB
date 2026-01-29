#!/bin/bash

# MementoDB 启动脚本
# 用法: ./start.sh [选项]
#   选项:
#     --build-only    只构建，不启动
#     --no-build      不构建，直接启动（如果未构建会失败）
#     --foreground,-f 前台运行（显示实时输出，Ctrl+C 停止）
#     --help,-h       显示帮助信息

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# 脚本目录（项目根目录）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 配置
BUILD_DIR="build"
EXECUTABLE="Debug/outDebug"
DATA_DIR="./data"
PID_FILE="./mementodb.pid"
LOG_FILE="./logs/server.log"

# 解析命令行参数
BUILD_ONLY=false
NO_BUILD=false
FOREGROUND=false

for arg in "$@"; do
    case $arg in
        --build-only)
            BUILD_ONLY=true
            shift
            ;;
        --no-build)
            NO_BUILD=true
            shift
            ;;
        --foreground|-f)
            FOREGROUND=true
            shift
            ;;
        --help|-h)
            echo "MementoDB 启动脚本"
            echo ""
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  --build-only    只构建项目，不启动服务器"
            echo "  --no-build      跳过构建，直接启动（如果未构建会失败）"
            echo "  --foreground,-f 前台运行（显示实时输出，Ctrl+C 停止）"
            echo "  --help, -h      显示此帮助信息"
            echo ""
            echo "示例:"
            echo "  $0              # 构建并启动服务器"
            echo "  $0 --build-only # 只构建"
            echo "  $0 --no-build   # 直接启动（假设已构建）"
            exit 0
            ;;
        *)
            echo -e "${RED}错误: 未知参数 '$arg'${NC}"
            echo "使用 --help 查看帮助信息"
            exit 1
            ;;
    esac
done

# 检测系统类型
detect_system() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        OS=$ID
        OS_VERSION=$VERSION_ID
    elif [ -f /etc/redhat-release ]; then
        OS="rhel"
        OS_VERSION=$(cat /etc/redhat-release | grep -oE '[0-9]+' | head -1)
    elif [ -f /etc/arch-release ]; then
        OS="arch"
    else
        OS="unknown"
    fi
}

# 获取安装命令
get_install_cmd() {
    case $OS in
        ubuntu|debian)
            if command -v apt-get &> /dev/null; then
                echo "sudo apt-get update && sudo apt-get install -y"
            else
                echo "sudo apt update && sudo apt install -y"
            fi
            ;;
        rhel|centos|fedora)
            if command -v dnf &> /dev/null; then
                echo "sudo dnf install -y"
            else
                echo "sudo yum install -y"
            fi
            ;;
        arch|manjaro)
            echo "sudo pacman -S --noconfirm"
            ;;
        opensuse*|suse)
            echo "sudo zypper install -y"
            ;;
        *)
            echo "unknown"
            ;;
    esac
}

# 检查并提示安装依赖
check_and_install_dependency() {
    local cmd=$1
    local package_name=$2
    local install_name=${3:-$package_name}
    
    if command -v "$cmd" &> /dev/null; then
        return 0
    fi
    
    echo -e "${YELLOW}警告: 未找到 $package_name${NC}"
    detect_system
    INSTALL_CMD=$(get_install_cmd)
    
    if [ "$INSTALL_CMD" = "unknown" ]; then
        echo -e "${RED}错误: 无法自动检测系统类型，请手动安装 $package_name${NC}"
        echo "   需要安装: $install_name"
        return 1
    fi
    
    echo -e "${CYAN}检测到系统: $OS${NC}"
    echo -e "${YELLOW}请运行以下命令安装 $package_name:${NC}"
    echo -e "${BLUE}  $INSTALL_CMD $install_name${NC}"
    echo ""
    
    # 根据系统提供具体的包名
    case $OS in
        ubuntu|debian)
            case $package_name in
                cmake)
                    echo -e "${BLUE}  或: sudo apt-get update && sudo apt-get install -y cmake build-essential${NC}"
                    ;;
                g++|clang++)
                    echo -e "${BLUE}  或: sudo apt-get update && sudo apt-get install -y build-essential${NC}"
                    ;;
                conan)
                    echo -e "${BLUE}  或: pip3 install conan${NC}"
                    ;;
            esac
            ;;
        rhel|centos|fedora)
            case $package_name in
                cmake)
                    echo -e "${BLUE}  或: sudo dnf install -y cmake gcc-c++ make${NC}"
                    ;;
                g++|clang++)
                    echo -e "${BLUE}  或: sudo dnf install -y gcc-c++ make${NC}"
                    ;;
                conan)
                    echo -e "${BLUE}  或: pip3 install conan${NC}"
                    ;;
            esac
            ;;
        arch|manjaro)
            case $package_name in
                cmake)
                    echo -e "${BLUE}  或: sudo pacman -S --noconfirm cmake base-devel${NC}"
                    ;;
                g++|clang++)
                    echo -e "${BLUE}  或: sudo pacman -S --noconfirm base-devel${NC}"
                    ;;
                conan)
                    echo -e "${BLUE}  或: pip3 install conan${NC}"
                    ;;
            esac
            ;;
    esac
    
    return 1
}

# 检查构建环境
check_build_environment() {
    local missing_deps=0
    
    echo -e "${BLUE}[1/5] 检查构建环境...${NC}"
    
    # 检查 CMake
    if ! command -v cmake &> /dev/null; then
        echo -e "${RED}✗ CMake 未安装${NC}"
        check_and_install_dependency "cmake" "CMake" "cmake"
        missing_deps=$((missing_deps + 1))
    else
        CMAKE_VERSION=$(cmake --version | head -n1 | cut -d' ' -f3)
        echo -e "${GREEN}✓ CMake 已安装 (版本: $CMAKE_VERSION)${NC}"
    fi
    
    # 检查 C++ 编译器
    local compiler_found=false
    local compiler_name=""
    
    if command -v g++ &> /dev/null; then
        compiler_found=true
        compiler_name="g++"
        GXX_VERSION=$(g++ --version | head -n1)
        echo -e "${GREEN}✓ g++ 已安装 ($GXX_VERSION)${NC}"
    elif command -v clang++ &> /dev/null; then
        compiler_found=true
        compiler_name="clang++"
        CLANG_VERSION=$(clang++ --version | head -n1)
        echo -e "${GREEN}✓ clang++ 已安装 ($CLANG_VERSION)${NC}"
    fi
    
    if [ "$compiler_found" = false ]; then
        echo -e "${RED}✗ C++ 编译器未安装（需要 g++ 或 clang++）${NC}"
        check_and_install_dependency "g++" "C++ 编译器" "build-essential"
        missing_deps=$((missing_deps + 1))
    fi
    
    # 检查 make
    if ! command -v make &> /dev/null; then
        echo -e "${YELLOW}⚠ make 未安装（某些系统可能需要）${NC}"
    else
        echo -e "${GREEN}✓ make 已安装${NC}"
    fi
    
    # 检查 CMake 版本（需要 3.16+）
    if command -v cmake &> /dev/null; then
        CMAKE_MAJOR=$(cmake --version | head -n1 | cut -d' ' -f3 | cut -d'.' -f1)
        CMAKE_MINOR=$(cmake --version | head -n1 | cut -d' ' -f3 | cut -d'.' -f2)
        if [ "$CMAKE_MAJOR" -lt 3 ] || ([ "$CMAKE_MAJOR" -eq 3 ] && [ "$CMAKE_MINOR" -lt 16 ]); then
            echo -e "${YELLOW}⚠ CMake 版本过低（当前: $CMAKE_MAJOR.$CMAKE_MINOR，需要 3.16+）${NC}"
        fi
    fi
    
    # 检查 Conan（可选）
    if [ -f "conanfile.txt" ]; then
        if command -v conan &> /dev/null; then
            CONAN_VERSION=$(conan --version 2>/dev/null || echo "unknown")
            echo -e "${GREEN}✓ Conan 已安装 (版本: $CONAN_VERSION)${NC}"
        else
            echo -e "${YELLOW}⚠ Conan 未安装（可选，用于依赖管理）${NC}"
            echo -e "${CYAN}  安装方法: pip3 install conan${NC}"
            echo -e "${CYAN}  或: curl -L https://github.com/conan-io/conan/releases/latest/download/conan-ubuntu-64.deb -o conan.deb && sudo dpkg -i conan.deb${NC}"
        fi
    fi
    
    if [ $missing_deps -gt 0 ]; then
        echo ""
        echo -e "${RED}错误: 缺少必需的依赖，请先安装上述依赖后重试${NC}"
        exit 1
    fi
    
    echo -e "${GREEN}✓ 构建环境检查完成${NC}\n"
}

# 清理旧的构建文件
clean_build_dir() {
    if [ -d "$BUILD_DIR" ]; then
        # 检查是否有 CMakeCache.txt（可能配置错误）
        if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
            echo -e "${YELLOW}检测到旧的构建缓存，清理中...${NC}"
            rm -rf "$BUILD_DIR"
        fi
    fi
    
    # 清理根目录的 CMake 文件（如果存在）
    if [ -f "CMakeCache.txt" ]; then
        echo -e "${YELLOW}清理根目录的 CMake 缓存...${NC}"
        rm -f CMakeCache.txt cmake_install.cmake
        rm -rf CMakeFiles/
    fi
}

# 检查是否已经运行
check_running() {
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE")
        if ps -p "$PID" > /dev/null 2>&1; then
            echo -e "${YELLOW}警告: MementoDB 已经在运行 (PID: $PID)${NC}"
            echo "使用 './stop.sh' 停止服务器，或使用 'kill $PID' 强制停止"
            exit 1
        else
            # PID 文件存在但进程不存在，删除过期的 PID 文件
            rm -f "$PID_FILE"
        fi
    fi
}

# 构建项目
build_project() {
    echo -e "${BLUE}[2/5] 清理旧的构建文件...${NC}"
    clean_build_dir
    
    echo -e "${BLUE}[3/5] 配置 CMake...${NC}"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    # 检查是否需要 Conan
    if [ -f "../conanfile.txt" ]; then
        if command -v conan &> /dev/null; then
            echo -e "${BLUE}检测到 Conan 配置文件，安装依赖...${NC}"
            if ! conan install .. --output-folder=. --build=missing 2>&1; then
                echo -e "${YELLOW}警告: Conan 安装失败，尝试不使用 Conan 构建...${NC}"
            fi
        else
            echo -e "${YELLOW}警告: 检测到 conanfile.txt 但未安装 Conan，跳过依赖安装${NC}"
            echo -e "${CYAN}  提示: 安装 Conan 可以获得更好的依赖管理${NC}"
        fi
    fi
    
    echo -e "${BLUE}运行 CMake 配置...${NC}"
    if ! cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1; then
        echo ""
        echo -e "${RED}错误: CMake 配置失败${NC}"
        echo -e "${YELLOW}请检查:${NC}"
        echo "  1. CMake 版本是否 >= 3.16"
        echo "  2. 编译器是否正确安装"
        echo "  3. 查看上面的错误信息"
        cd ..
        exit 1
    fi
    
    echo -e "${BLUE}[4/5] 编译项目...${NC}"
    # 检测 CPU 核心数
    if command -v nproc &> /dev/null; then
        CORES=$(nproc)
    elif [ -f /proc/cpuinfo ]; then
        CORES=$(grep -c processor /proc/cpuinfo)
    else
        CORES=4
    fi
    
    echo -e "${CYAN}使用 $CORES 个并行任务编译...${NC}"
    if ! cmake --build . -j$CORES 2>&1; then
        echo ""
        echo -e "${RED}错误: 编译失败${NC}"
        echo -e "${YELLOW}请检查:${NC}"
        echo "  1. 源代码是否有错误"
        echo "  2. 依赖是否完整安装"
        echo "  3. 查看上面的错误信息"
        cd ..
        exit 1
    fi
    
    cd ..
    
    if [ ! -f "$BUILD_DIR/$EXECUTABLE" ]; then
        echo -e "${RED}错误: 可执行文件不存在: $BUILD_DIR/$EXECUTABLE${NC}"
        echo -e "${YELLOW}可能的原因:${NC}"
        echo "  1. 编译过程中出现错误"
        echo "  2. 可执行文件输出路径配置错误"
        echo "  3. 请查看上面的编译输出"
        exit 1
    fi
    
    echo -e "${GREEN}✓ 构建完成${NC}"
    echo -e "${CYAN}可执行文件位置: $BUILD_DIR/$EXECUTABLE${NC}\n"
}

# 准备数据目录
prepare_data_dir() {
    echo -e "${BLUE}[5/5] 准备数据目录...${NC}"
    mkdir -p "$DATA_DIR"
    mkdir -p "$DATA_DIR/wal"
    mkdir -p "$(dirname "$LOG_FILE")"
    echo -e "${GREEN}✓ 数据目录准备完成${NC}\n"
}

# 启动服务器
start_server() {
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}  启动 MementoDB 服务器${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo -e "数据目录: ${BLUE}$DATA_DIR${NC}"
    echo -e "日志文件: ${BLUE}$LOG_FILE${NC}"
    echo -e "监听地址: ${BLUE}127.0.0.1:6380${NC}"
    echo ""
    
    # 检查可执行文件
    if [ ! -f "$BUILD_DIR/$EXECUTABLE" ]; then
        echo -e "${RED}错误: 可执行文件不存在: $BUILD_DIR/$EXECUTABLE${NC}"
        echo "请先运行构建: $0 --build-only"
        exit 1
    fi
    
    # 检查端口是否被占用
    if command -v netstat &> /dev/null; then
        if netstat -tuln 2>/dev/null | grep -q ":6380 "; then
            echo -e "${YELLOW}警告: 端口 6380 可能已被占用${NC}"
        fi
    elif command -v ss &> /dev/null; then
        if ss -tuln 2>/dev/null | grep -q ":6380 "; then
            echo -e "${YELLOW}警告: 端口 6380 可能已被占用${NC}"
        fi
    fi
    
    cd "$BUILD_DIR"
    
    if [ "$FOREGROUND" = true ]; then
        # 前台运行
        echo -e "${GREEN}前台运行模式，按 Ctrl+C 停止服务器${NC}"
        echo ""
        cd ..
        # 设置退出时清理 PID 文件
        trap 'rm -f "$PID_FILE"; exit 0' INT TERM
        # 直接运行（阻塞）
        ./$BUILD_DIR/$EXECUTABLE
        rm -f "$PID_FILE"
    else
        # 后台运行
        echo -e "${YELLOW}提示: 使用 Ctrl+C 停止服务器${NC}"
        echo ""
        ./$EXECUTABLE > "../$LOG_FILE" 2>&1 &
        SERVER_PID=$!
        cd ..
        
        echo "$SERVER_PID" > "$PID_FILE"
        
        # 等待一下，检查进程是否还在运行
        sleep 2
        if ! ps -p "$SERVER_PID" > /dev/null 2>&1; then
            rm -f "$PID_FILE"
            echo -e "${RED}错误: 服务器启动失败${NC}"
            echo -e "${YELLOW}请查看日志文件获取详细信息:${NC}"
            echo -e "${BLUE}  tail -n 50 $LOG_FILE${NC}"
            exit 1
        fi
        
        echo -e "${GREEN}✓ 服务器已启动 (PID: $SERVER_PID)${NC}"
        echo ""
        echo -e "${CYAN}=== 连接数据库 ===${NC}"
        echo -e "使用以下命令连接:"
        echo -e "  ${BLUE}redis-cli -p 6380${NC}"
        echo ""
        echo -e "或使用 telnet:"
        echo -e "  ${BLUE}telnet 127.0.0.1 6380${NC}"
        echo ""
        echo -e "${CYAN}=== 管理命令 ===${NC}"
        echo -e "停止服务器: ${BLUE}./stop.sh${NC} 或 ${BLUE}kill $SERVER_PID${NC}"
        echo -e "查看日志: ${BLUE}tail -f $LOG_FILE${NC}"
        echo -e "查看进程: ${BLUE}ps aux | grep outDebug${NC}"
        echo ""
    fi
}

# 主流程
main() {
    echo -e "${CYAN}"
    echo "╔════════════════════════════════════════╗"
    echo "║      MementoDB 启动脚本                ║"
    echo "╚════════════════════════════════════════╝"
    echo -e "${NC}\n"
    
    check_running
    
    if [ "$BUILD_ONLY" = true ]; then
        check_build_environment
        build_project
        prepare_data_dir
        echo -e "${GREEN}构建完成！使用 '$0' 启动服务器${NC}"
        exit 0
    fi
    
    # 检查是否需要构建
    if [ "$NO_BUILD" = false ]; then
        if [ ! -f "$BUILD_DIR/$EXECUTABLE" ]; then
            echo -e "${YELLOW}可执行文件不存在，开始构建...${NC}\n"
            check_build_environment
            build_project
        else
            echo -e "${GREEN}✓ 可执行文件已存在，跳过构建${NC}\n"
        fi
    fi
    
    prepare_data_dir
    start_server
}

# 捕获 Ctrl+C（仅用于非前台模式）
if [ "$FOREGROUND" = false ]; then
    trap 'echo -e "\n${YELLOW}正在停止服务器...${NC}"; exit 0' INT TERM
fi

# 运行主流程
main
