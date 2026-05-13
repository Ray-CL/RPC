# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

Krpc 是一个基于 C++11 的轻量级 RPC 框架，使用 Muduo 网络库实现 Reactor 模式，Protobuf 做序列化，ZooKeeper 做服务注册与发现。

## 构建命令

```bash
# 创建 build 目录并构建
mkdir -p build && cd build
cmake .. && make -j$(nproc)

# 构建产物输出到项目根目录的 bin/ 下：
# - bin/server：服务端示例（example/callee/）
# - bin/client：客户端示例（example/caller/）
# - src/libkrpc_core.a：核心静态库
```

## 运行方式

```bash
# 1. 确保 ZooKeeper 服务在运行（默认 127.0.0.1:2181）

# 2. 启动服务端（需要配置文件，多个实例用不同端口）
cd bin
./server -i ./test.conf &    # 后台启动，端口 8000
./server -i ./test1.conf &   # 后台启动，端口 8001（如需多实例）
./server -i ./test2.conf &   # 后台启动，端口 8002（如需多实例）
./client -i ./test.conf      # 前台启动客户端（压力测试，8线程 × 5000请求）

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

- **`KrpcApplication`**（单例）：框架入口，解析命令行（`-i <config>`）并加载配置文件到 `Krpcconfig`
- **`Krpcconfig`**：配置文件解析器，`LoadConfigFile()` 读取 `key=value` 格式文件，`Load()` 按 key 查询
- **`KrpcLogger`**：RAII 封装 glog，构造时自动 `InitGoogleLogging`，析构时 `ShutdownGoogleLogging`。提供静态 `Info/Warning/ERROR/Fatal` 方法
- **`KrpcProvider`**：服务端核心。`NotifyService()` 注册 protobuf Service，`Run()` 启动 muduo TcpServer 并将服务注册到 ZooKeeper
- **`KrpcChannel`**：客户端核心，继承自 `google::protobuf::RpcChannel`。`CallMethod()` 负责从 ZooKeeper 发现服务、通过连接池复用 TCP 连接、序列化请求/反序列化响应、故障重试
- **`Krpccontroller`**：继承自 `google::protobuf::RpcController`，跟踪 RPC 调用状态和错误信息
- **`ZkClient`**：ZooKeeper 客户端封装，提供 `GetData()`/`GetChildrenWithWatcher()`/`Create()` 方法。`Start()` 用条件变量等待 ZK 连接成功

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

**刷新机制（双重触发）：**
1. **Watcher 推送**：`GetChildrenWithWatcher()` 注册子节点变化监听，ZK 节点变更时回调 `ChildWatcher` 设置脏标记 `m_providers_dirty`，下次 `CallMethod` 时触发刷新
2. **定时兜底**：每 10 秒强制刷新一次，防止 watcher 丢失

**连接池：** 每个 `KrpcChannel` 维护 `m_conn_pool`（key = `"ip:port"`）。`CallMethod` 每次通过 `ConnectToAvailableProvider` 随机选择存活节点后优先查池复用已有连接；单实例和多实例均复用。失败时 `RemoveFromPool` 关闭坏连接；`RefreshProviders` 刷新后同步清理池中已下线实例的失效连接。

**故障隔离：** 3 秒内不重试已失败的节点（`ProviderAlive()` 检查）。`RefreshProviders()` 保留旧节点的故障状态。

### 服务端反射分发与回调

`OnMessage` 解包后，通过 protobuf 反射动态创建请求/响应对象，不依赖具体类型：
```
request  = service->GetRequestPrototype(method).New()   // 动态 new LoginRequest
response = service->GetResponsePrototype(method).New()  // 动态 new LoginResponse
done     = NewCallback(this, &SendRpcResponse, conn, response)
service->CallMethod(method, nullptr, request, response, done)
```

调用链：`CallMethod`（虚函数）→ protobuf 生成的 `switch(method->index())` 分派 → 用户重写的 `Login(controller, request, response, done)` → 填 response → `done->Run()` → `SendRpcResponse(conn, response)` 序列化并发送。

### 客户端调用链与重试

`stub.Login(...)` → protobuf Stub 转发 → `KrpcChannel::CallMethod()`：
1. 服务发现（ZK/连接池）→ 2. 序列化请求 → 3. 打包 `[TotalLen][HeaderLen][Header][Args]` → 4. send → 5. recv_exact 收响应 → 6. 反序列化

最多 2 次重试（`for (int attempt = 0; attempt < 2; ++attempt)`），任何步骤失败：标记节点 failed → 从连接池移除坏连接 → `continue` 重试不同存活节点。

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
