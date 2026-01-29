# Index 模块测试说明

## 编译测试程序

```bash
cd /home/w/MementoDB
mkdir -p build
cd build
cmake ..
make test_index
```

## 运行测试

```bash
./src/index/test_index
```

或者从构建目录：

```bash
./test_index
```

## 测试内容

测试程序包含以下测试用例：

### 1. HashIndex 测试
- ✓ 插入操作
- ✓ 查找操作
- ✓ contains 操作
- ✓ 更新操作
- ✓ 删除操作
- ✓ size 和 empty 操作
- ✓ 批量插入
- ✓ insert_or_update 操作
- ✓ 统计信息获取
- ✓ 负载因子计算
- ✓ 清空操作

### 2. SkipListIndex 测试
- ✓ 插入操作
- ✓ 查找操作
- ✓ 范围查询
- ✓ 前缀查询
- ✓ min/max 操作
- ✓ 迭代器操作
- ✓ 批量插入
- ✓ 内存使用统计
- ✓ 统计信息获取

### 3. BloomFilter 测试
- ✓ 插入操作
- ✓ 存在性检查
- ✓ 批量插入
- ✓ 假阳性率计算
- ✓ 位使用率计算
- ✓ 估计元素数量
- ✓ 统计信息获取
- ✓ 清空操作

### 4. Counting BloomFilter 测试
- ✓ 插入操作
- ✓ 删除操作（计数布隆过滤器支持）
- ✓ 删除支持检查

### 5. IndexManager 测试
- ✓ 创建哈希索引
- ✓ 创建跳表索引
- ✓ 创建布隆过滤器
- ✓ 获取索引
- ✓ 检查索引存在性
- ✓ 列出索引
- ✓ 获取所有统计信息
- ✓ 获取管理器指标
- ✓ 删除索引

### 6. 性能测试
- HashIndex 插入性能
- SkipListIndex 插入性能
- BloomFilter 插入性能

## 预期输出

测试程序会输出详细的测试结果，包括：
- 每个测试用例的执行状态（✓ 或 ✗）
- 性能测试的耗时统计
- 最终的测试总结

## 注意事项

1. 测试会在 `./test_indexes` 目录创建临时文件
2. 测试完成后可以手动清理该目录
3. 如果测试失败，会显示详细的错误信息

