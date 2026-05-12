# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

Krpc 是一个基于 C++11 的轻量级 RPC 框架，使用 Muduo 网络库实现 Reactor 模式，Protobuf 做序列化，ZooKeeper 做服务注册与发现。

## 构建命令

```bash
# 创建 build 目录并构建
mkdir -p build && cd build
cmake .. && make -j$(nproc)

# 构建产物
# - bin/server：服务端示例（example/callee/）
# - bin/client：客户端示例（example/caller/）
# - libkrpc_core.a：核心静态库
```

## 运行方式

```bash
# 1. 确保 ZooKeeper 服务在运行（默认 127.0.0.1:2181）

# 2. 启动服务端（需要配置文件）
./bin/server -i bin/test.conf

# 3. 启动客户端（压力测试，可多进程并发）
./bin/client -i bin/test.conf
```

配置文件格式为 `key=value`，支持以下键：

| 键 | 说明 |
|---|---|
| `rpcserverip` | RPC 服务绑定 IP |
| `rpcserverport` | RPC 服务绑定端口 |
| `zookeeperip` | ZooKeeper 地址 |
| `zookeeperport` | ZooKeeper 端口 |

## 核心架构

### 分层结构

```
example/callee (服务实现)     example/caller (调用方)
       ↓                              ↓
KrpcProvider (服务端网络层)    KrpcChannel (客户端网络层)
       ↓                              ↓
  Muduo TcpServer              raw TCP socket
       ↓                              ↓
 ZkClient (服务注册)          ZkClient (服务发现)
       ↓                              ↓
   ZooKeeper ←──────────────→ ZooKeeper
```

### 关键类

- **`KrpcApplication`**（单例）：框架入口，解析命令行（`-i <config>`）并加载配置文件
- **`KrpcProvider`**：服务端核心。`NotifyService()` 注册 protobuf Service，`Run()` 启动 muduo TcpServer 并将服务注册到 ZooKeeper
- **`KrpcChannel`**：客户端核心，继承自 `google::protobuf::RpcChannel`。`CallMethod()` 负责从 ZooKeeper 发现服务、建立连接、序列化请求/反序列化响应、故障重试
- **`Krpccontroller`**：继承自 `google::protobuf::RpcController`，跟踪 RPC 调用状态和错误信息
- **`ZkClient`**：ZooKeeper 客户端封装，提供 `GetData()`/`GetChildren()`/`Create()` 方法

### 自定义协议格式

请求包：
```
[4B TotalLen(net)] + [4B HeaderLen(net)] + [Protobuf RpcHeader] + [Protobuf Args]
```

响应包：
```
[4B TotalLen(net)] + [Protobuf ResponseData]
```

`RpcHeader` 定义在 `src/Krpcheader.proto`，包含 `service_name`、`method_name`、`args_size`。

### ZooKeeper 服务发现

ZooKeeper 路径结构：`/服务名/方法名/ip:port`

- 服务名、方法名为永久节点
- `ip:port` 实例节点为临时节点（`ZOO_EPHEMERAL`），服务端断开后自动删除

客户端每 10 秒刷新一次提供者列表，支持多实例随机负载均衡 + 3 秒故障隔离。

### 添加新 RPC 服务的流程

1. 定义 `.proto` 文件，设置 `cc_generic_services = true`
2. 用 protoc 生成 C++ 代码：`protoc --cpp_out=. your_service.proto`
3. 服务端：继承生成的 `ServiceRpc` 基类，重写虚函数，在 `main` 中 `provider.NotifyService(new YourService())` 后调用 `provider.Run()`
4. 客户端：使用生成的 `Stub` 类，传入 `new KrpcChannel(false)`，调用 stub 方法即可

## 依赖库

- **protobuf**（序列化）
- **muduo**（net + base，Reactor 网络库）
- **zookeeper_mt**（ZooKeeper C 客户端多线程版）
- **glog**（日志）
- **pthread**（线程）
