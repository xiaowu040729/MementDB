# 第三方依赖说明

本项目使用以下第三方库：

## 1. CRC32C (crc32c)

**用途**: CRC32 校验和计算（支持硬件加速）

**版本**: 1.1.2

**用途场景**:
- WAL 日志校验
- 数据完整性校验
- 关键路径的数据验证

**安装方式**:
```bash
# 使用 Conan
conan install . --build=missing

# 或手动安装
# 参考: https://github.com/google/crc32c
```

## 2. xxHash (xxhash)

**用途**: 高性能哈希算法

**版本**: 0.8.2

**用途场景**:
- 哈希表键值计算
- 数据分片
- 布隆过滤器
- 快速哈希计算

**安装方式**:
```bash
# 使用 Conan
conan install . --build=missing

# 或手动安装
# 参考: https://github.com/Cyan4973/xxHash
```

## 3. Zstandard (zstd)

**用途**: 数据压缩/解压

**版本**: 1.5.5

**用途场景**:
- WAL 日志压缩
- 数据存储压缩
- 网络传输压缩

**安装方式**:
```bash
# 使用 Conan
conan install . --build=missing

# 或手动安装
# 参考: https://github.com/facebook/zstd
```

## 构建说明

### 方式一：使用 Conan（推荐，但需要先安装）

**步骤 1: 安装 pip 和 Conan**
```bash
# 安装 pip（如果没有）
sudo apt update
sudo apt install -y python3-pip

# 安装 Conan
pip3 install --user conan
export PATH=$PATH:~/.local/bin  # 将 Conan 添加到 PATH

# 配置 Conan（首次使用）
conan profile detect --force
```

**步骤 2: 安装依赖**
```bash
# 在项目根目录执行
conan install . --build=missing -s build_type=Release
```

**步骤 3: 配置和编译 CMake**
```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake
cmake --build build
```

### 方式二：使用系统包管理器（Ubuntu/Debian）

如果系统包管理器有这些库，可以直接安装：

```bash
# 安装开发包
sudo apt update
sudo apt install -y \
    libzstd-dev \
    libxxhash-dev

# crc32c 可能需要从源码编译或使用 Conan
# 如果系统没有，建议使用 Conan 安装
```

然后直接使用 CMake 构建（CMakeLists.txt 中的 `find_package` 会自动查找）：
```bash
cmake -S . -B build
cmake --build build
```

### 方式三：手动编译安装（高级用户）

如果上述方式都不行，可以手动编译安装（参考各库的 GitHub 仓库）。

**注意**: 更多详细说明请参考 `INSTALL.md` 文件。

## 使用示例

### CRC32 校验
```cpp
#include "utils/CRC32.hpp"

uint32_t crc = mementodb::utils::CRC32::Compute(data, size);
```

### xxHash 哈希
```cpp
#include "utils/Hash.hpp"

uint32_t hash32 = utils::Hash::ComputeHashWith(
    utils::Hash::XXHASH_32, data, size
);
uint64_t hash64 = utils::Hash::ComputeHash64(data, size);
```

### Zstd 压缩
```cpp
#include "utils/Compression.hpp"

auto compressed = mementodb::utils::Compression::Compress(
    data, size, 
    mementodb::utils::Compression::Level::DEFAULT
);
auto decompressed = mementodb::utils::Compression::Decompress(
    compressed->data(), compressed->size()
);
```

