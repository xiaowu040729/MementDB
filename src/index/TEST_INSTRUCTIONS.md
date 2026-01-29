# Index 模块测试说明

## 问题说明

测试程序需要编译，但项目依赖 Conan 管理的第三方库（xxhash, crc32c, zstd）。

## 解决方案

### 方案 1：使用 Conan 安装依赖（推荐）

```bash
# 1. 安装 Conan（如果还没有）
pip install conan

# 2. 安装依赖
cd /home/w/MementoDB
conan install . --output-folder=build --build=missing

# 3. 配置 CMake（使用 Conan 工具链）
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake

# 4. 编译测试程序
make test_index

# 5. 运行测试
./test_index
```

### 方案 2：修改代码以不依赖 xxhash（临时方案）

如果不想使用 Conan，可以修改 `BloomFilter.cpp` 中的 `ComputeHash64` 调用，改用 `ComputeHash`（32位版本），但这会影响 BloomFilter 的性能测试。

### 方案 3：使用系统包管理器安装依赖

```bash
# Ubuntu/Debian
sudo apt-get install libxxhash-dev libcrc32c-dev libzstd-dev

# 然后修改 CMakeLists.txt 使用系统库而不是 Conan
```

## 当前状态

- ✅ Index 模块代码已完成
- ✅ 测试程序已创建（test_index.cpp）
- ✅ CMakeLists.txt 已配置
- ⚠️ 需要解决第三方库依赖问题才能编译

## 测试内容

测试程序包含以下测试：

1. **HashIndex 测试** - 哈希索引的所有功能
2. **SkipListIndex 测试** - 跳表索引的所有功能（包括范围查询）
3. **BloomFilter 测试** - 布隆过滤器的所有功能
4. **Counting BloomFilter 测试** - 计数布隆过滤器
5. **IndexManager 测试** - 索引管理器功能
6. **性能测试** - 各索引类型的插入性能

## 快速检查代码

即使无法编译，你也可以检查代码质量：

```bash
# 检查语法错误
cd /home/w/MementoDB
find src/index -name "*.cpp" -o -name "*.hpp" | xargs -I {} cppcheck --enable=all {} 2>&1 | grep -i error

# 检查代码行数
wc -l src/index/*.cpp src/index/*.hpp
```

## 下一步

1. 安装 Conan 和依赖
2. 编译测试程序
3. 运行测试验证功能
4. 根据测试结果优化代码

