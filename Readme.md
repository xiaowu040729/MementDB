# MementoDB 快速开始指南

## 一键启动

**在任何 Linux 系统上，只需运行：**

```bash
./start.sh
```

脚本会自动：
1. ✅ 检测系统类型（Ubuntu/Debian/CentOS/Arch等）
2. ✅ 检查必需的依赖（CMake、编译器）
3. ✅ 提供缺失依赖的安装命令
4. ✅ 自动构建项目
5. ✅ 启动数据库服务器

## 系统要求

### 必需依赖

- **CMake** 3.16 或更高版本
- **C++ 编译器** (g++ 或 clang++)
- **make** (通常随编译器一起安装)

### 可选依赖

- **Conan** (用于依赖管理，可选)

## 自动安装依赖

如果缺少依赖，脚本会显示安装命令。根据你的系统类型：

### Ubuntu/Debian
```bash
sudo apt-get update
sudo apt-get install -y cmake build-essential
```

### CentOS/RHEL/Fedora
```bash
sudo dnf install -y cmake gcc-c++ make
# 或使用 yum (旧版本)
sudo yum install -y cmake gcc-c++ make
```

### Arch Linux
```bash
sudo pacman -S --noconfirm cmake base-devel
```

### 安装 Conan (可选)
```bash
pip3 install conan
```

## 使用示例

### 1. 首次启动（自动构建）
```bash
./start.sh
```

### 2. 前台运行（查看实时日志）
```bash
./start.sh --foreground
```

### 3. 只构建，不启动
```bash
./start.sh --build-only
```

### 4. 直接启动（跳过构建）
```bash
./start.sh --no-build
```

## 连接数据库

服务器启动后，监听在 `127.0.0.1:6380`

### 使用 redis-cli
```bash
redis-cli -p 6380
```

### 使用 telnet
```bash
telnet 127.0.0.1 6380
```

### 基本操作
```bash
127.0.0.1:6380> SET mykey "Hello"
OK
127.0.0.1:6380> GET mykey
"Hello"
127.0.0.1:6380> EXISTS mykey
(integer) 1
127.0.0.1:6380> DEL mykey
(integer) 1
```

## 停止服务器

```bash
./stop.sh
```

或如果在前台运行，按 `Ctrl+C`

## 查看日志

```bash
tail -f logs/server.log
```

