#!/usr/bin/env python3
"""
MementoDB Python 客户端示例

使用前请安装 redis 库:
    pip install redis

启动服务器:
    ./start.sh
"""

import redis
import sys
import time

def main():
    # 连接到 MementoDB 服务器
    print("=== MementoDB Python 客户端示例 ===\n")
    
    try:
        # 创建 Redis 客户端（连接到 MementoDB）
        r = redis.Redis(
            host='127.0.0.1',
            port=6380,
            decode_responses=True,  # 自动解码字符串
            socket_connect_timeout=5
        )
        
        # 测试连接
        print("1. 测试连接...")
        pong = r.ping()
        print(f"   PING: {pong}\n")
        
        # 基本操作
        print("2. 基本键值操作...")
        r.set('greeting', 'Hello MementoDB!')
        value = r.get('greeting')
        print(f"   SET greeting 'Hello MementoDB!'")
        print(f"   GET greeting: {value}\n")
        
        # 批量设置
        print("3. 批量操作...")
        data = {
            'user:1:name': 'Alice',
            'user:1:email': 'alice@example.com',
            'user:1:age': '30',
            'user:2:name': 'Bob',
            'user:2:email': 'bob@example.com',
            'user:2:age': '25'
        }
        r.mset(data)
        print(f"   MSET {len(data)} 个键值对")
        
        # 批量获取
        keys = list(data.keys())
        values = r.mget(keys)
        print(f"   MGET {len(keys)} 个键")
        for key, val in zip(keys, values):
            print(f"     {key}: {val}")
        print()
        
        # 检查存在
        print("4. 检查键是否存在...")
        exists = r.exists('greeting')
        print(f"   EXISTS greeting: {exists}")
        exists = r.exists('nonexistent')
        print(f"   EXISTS nonexistent: {exists}\n")
        
        # 删除操作
        print("5. 删除操作...")
        deleted = r.delete('greeting')
        print(f"   DELETE greeting: {deleted}")
        value = r.get('greeting')
        print(f"   GET greeting (after delete): {value}\n")
        
        # 使用管道提高性能
        print("6. 使用管道批量操作...")
        pipe = r.pipeline()
        for i in range(10):
            pipe.set(f'pipe:key{i}', f'value{i}')
        results = pipe.execute()
        print(f"   通过管道设置了 {len(results)} 个键")
        
        # 验证
        value = r.get('pipe:key5')
        print(f"   GET pipe:key5: {value}\n")
        
        # 事务示例
        print("7. 事务操作...")
        pipe = r.pipeline()
        pipe.multi()
        pipe.set('tx:key1', 'value1')
        pipe.set('tx:key2', 'value2')
        pipe.set('tx:key3', 'value3')
        pipe.execute()
        results = pipe.execute()
        print(f"   事务执行结果: {results}\n")
        
        # 服务器信息
        print("8. 服务器信息...")
        try:
            info = r.info()
            print(f"   服务器版本: {info.get('redis_version', 'N/A')}")
            print(f"   已使用内存: {info.get('used_memory_human', 'N/A')}")
        except Exception as e:
            print(f"   获取信息失败: {e}")
        print()
        
        # 性能测试
        print("9. 简单性能测试...")
        num_keys = 1000
        start_time = time.time()
        
        pipe = r.pipeline()
        for i in range(num_keys):
            pipe.set(f'perf:key{i}', f'value{i}')
        pipe.execute()
        
        elapsed = time.time() - start_time
        ops_per_sec = num_keys / elapsed
        print(f"   设置 {num_keys} 个键")
        print(f"   耗时: {elapsed:.2f} 秒")
        print(f"   吞吐量: {ops_per_sec:.0f} ops/sec\n")
        
        print("=== 示例完成 ===")
        
    except redis.ConnectionError as e:
        print(f"错误: 无法连接到服务器 ({e})")
        print("请确保 MementoDB 服务器正在运行:")
        print("  ./start.sh")
        sys.exit(1)
    except Exception as e:
        print(f"错误: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

if __name__ == '__main__':
    main()
