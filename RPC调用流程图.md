# RPC 调用完整流程图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           客  户  端                                        │
│                                                                            │
│  stub.Login(&controller, &request, &response, nullptr)                     │
│    │                                                                       │
│    ▼                                                                       │
│  ┌─ UserServiceRpc_Stub::Login() ── user.pb.cc:1526 ──┐                   │
│  │  protobuf 自动生成，纯转发：                        │                   │
│  │  channel_->CallMethod(descriptor()->method(0),      │                   │
│  │                       controller, request,          │                   │
│  │                       response, done)               │                   │
│  └────────────────────────┬────────────────────────────┘                   │
│                           │                                                │
│                           ▼                                                │
│  ┌─ KrpcChannel::CallMethod() ── Krpcchannel.cc:78 ──┐                    │
│  │                                                     │                    │
│  │  ① EnsureZkClient() — 懒初始化 ZK 客户端           │                    │
│  │     └─ new ZkClient → Start() → 阻塞等 ZK 连上     │                    │
│  │                                                     │                    │
│  │  ② ConnectToAvailableProvider()                     │                    │
│  │     ├─ RefreshProviders()  ← ZK Watcher/定时触发   │                    │
│  │     │   └─ zkclient->GetChildrenWithWatcher(        │                    │
│  │     │        "/Kuser.UserServiceRpc/Login",         │                    │
│  │     │        ChildWatcher, this)                    │                    │
│  │     │     → 返回 ["127.0.0.1:8000"]                │                    │
│  │     │                                               │                    │
│  │     ├─ while (SelectProvider(ip, port))             │                    │
│  │     │     随机筛存活节点                             │                    │
│  │     │                                               │                    │
│  │     ├─ GetPooledFd(ip, port) → 池里有？复用！       │                    │
│  │     │                                               │                    │
│  │     └─ newConnect(ip, port)                         │                    │
│  │         └─ socket() → connect() → AddToPool()       │                    │
│  │                                                     │                    │
│  │  ③ request->SerializeToString() → args_str         │                    │
│  │     LoginRequest{name:"zhangsan", pwd:"123456"}     │                    │
│  │                                                     │                    │
│  │  ④ 构建 RpcHeader{service_name, method_name,        │                    │
│  │                    args_size} 并序列化               │                    │
│  │                                                     │                    │
│  │  ⑤ 打包发送                                        │                    │
│  │  ┌────────┬────────┬──────────────┬───────────┐    │                    │
│  │  │TotalLen│HdrLen  │ RpcHeader    │ LoginReq  │    │                    │
│  │  │ 4B 网络│ 4B 网络│ protobuf     │ protobuf  │    │                    │
│  │  └────────┴────────┴──────────────┴───────────┘    │                    │
│  │  → send(fd, ...)                                    │                    │
│  │                                                     │                    │
│  │  ⑥ recv_exact(fd, &response_len, 4)                │    阻塞等响应     │
│  │     recv_exact(fd, buf, response_len)               │                    │
│  │                                                     │                    │
│  │  ⑦ response->ParseFromArray(buf, len)              │                    │
│  │     → LoginResponse{errcode=0, success=true}        │                    │
│  │                                                     │                    │
│  │  失败任何一步 → MarkProviderFailed → RemoveFromPool │                    │
│  │              → continue（最多重试 2 次）            │                    │
│  └─────────────────────────────────────────────────────┘                    │
│                                                                            │
│                        ║  TCP 网络  ║                                      │
│                        ║            ║                                      │
└────────────────────────╬────────────╬──────────────────────────────────────┘
                         ║            ║
┌────────────────────────╬────────────╬──────────────────────────────────────┐
│                        ║  服  务  端 ║                                      │
│                        ║            ║                                      │
│                        ║  muduo epoll 收到数据                             │
│                        ║            │                                      │
│                        ║            ▼                                      │
│  ┌─ KrpcProvider::OnMessage(conn, buffer, timestamp) ── Krpcprovider.cc ──┐│
│  │                                                                         ││
│  │  while (buffer->readableBytes() >= 4) {  ← 拆包/粘包处理               ││
│  │                                                                         ││
│  │    解包：                                                               ││
│  │    ├─ peek 4B → total_len                                              ││
│  │    ├─ 判断可读 >= 4+total_len？否→break 等下次拼接                      ││
│  │    ├─ retrieve 4B total_len                                            ││
│  │    ├─ read 4B → header_len                                             ││
│  │    ├─ read header → Parse → RpcHeader                                  ││
│  │    │   → service_name="Kuser.UserServiceRpc"                           ││
│  │    │   → method_name ="Login"                                          ││
│  │    └─ read args → args_str                                             ││
│  │                                                                         ││
│  │    查找 service_map：                                                   ││
│  │    ├─ find("Kuser.UserServiceRpc") → ServiceInfo                       ││
│  │    │   └─ method_map.find("Login") → MethodDescriptor                  ││
│  │    │                                                                   ││
│  │    └─ 反射创建：                                                        ││
│  │       request  = service->GetRequestPrototype(method).New()            ││
│  │                → new LoginRequest()  ← 动态，不依赖具体类型             ││
│  │       request->ParseFromString(args_str)                               ││
│  │                → LoginRequest{name:"zhangsan", pwd:"123456"}           ││
│  │                                                                         ││
│  │       response = service->GetResponsePrototype(method).New()           ││
│  │                → new LoginResponse()  ← 空的，等业务填充                ││
│  │                                                                         ││
│  │       ┌───────────────────────────────────────────────────────────┐    ││
│  │       │  ★★★ 回调闭包创建 ★★★                                     │    ││
│  │       │                                                            │    ││
│  │       │  done = NewCallback<KrpcProvider,                          │    ││
│  │       │                     const TcpConnectionPtr&,               │    ││
│  │       │                     Message*>(                             │    ││
│  │       │      this,           ← KrpcProvider*                       │    ││
│  │       │      &KrpcProvider::SendRpcResponse, ← 成员函数指针        │    ││
│  │       │      conn,           ← 这个 TCP 连接                       │    ││
│  │       │      response)       ← 空的响应对象（等业务填充）           │    ││
│  │       │                                                            │    ││
│  │       │  → new MethodClosure2<KrpcProvider, TcpConnectionPtr&,     │    ││
│  │       │                        Message*>   ← callback.h:234        │    ││
│  │       │                                                            │    ││
│  │       │  MethodClosure2 内存了三个值：                              │    ││
│  │       │    object_  = KrpcProvider 的 this 指针                    │    ││
│  │       │    method_  = &KrpcProvider::SendRpcResponse               │    ││
│  │       │    arg1_    = conn                                         │    ││
│  │       │    arg2_    = response  ← 还是空的，等业务代码填！          │    ││
│  │       │                                                            │    ││
│  │       │  此时 Run() 不会执行！只是 new 了一个对象存起来。           │    ││
│  │       │  Run() 里面只有一行：                                      │    ││
│  │       │    (object_->*method_)(arg1_, arg2_)                       │    ││
│  │       │    = provider->SendRpcResponse(conn, response)             │    ││
│  │       └───────────────────────────────────────────────────────────┘    ││
│  │                                                                         ││
│  │    ┌─ service->CallMethod(method, nullptr, request, response, done) ─┐  ││
│  │    │   service 静态类型是 Service*，实际指向 UserService 对象        │  ││
│  │    │   CallMethod 是虚函数 → 多态！                                  │  ││
│  │    │                                                                  │  ││
│  │    │   ┌─ UserServiceRpc::CallMethod() ── user.pb.cc:1456 ──┐        │  ││
│  │    │   │  protobuf 生成的分发器                               │        │  ││
│  │    │   │                                                      │        │  ││
│  │    │   │  switch(method->index()) {                           │        │  ││
│  │    │   │    case 0:  ← Login 在 proto 中排第 0               │        │  ││
│  │    │   │      Login(controller,                               │        │  ││
│  │    │   │            DownCast<LoginRequest*>(request),         │        │  ││
│  │    │   │            DownCast<LoginResponse*>(response),       │        │  ││
│  │    │   │            done);                                    │        │  ││
│  │    │   │      break;                                         │        │  ││
│  │    │   │    case 1: Register(...)                             │        │  ││
│  │    │   │  }                                                  │        │  ││
│  │    │   └─────────────────────────────────────────────────────┘        │  ││
│  │    │       │                                                          │  ││
│  │    │       │  Login() 也是虚函数 → 多态！                              │  ││
│  │    │       ▼                                                          │  ││
│  │    │  ┌─ UserService::Login(controller, request, response, done) ─┐   │  ││
│  │    │  │              ↑ Kserver.cc:26  用户重写的业务逻辑  ↑        │   │  ││
│  │    │  │                                                           │   │  ││
│  │    │  │  // 第1步：从 request 读取客户端参数                      │   │  ││
│  │    │  │  string name = request->name();  // "zhangsan"            │   │  ││
│  │    │  │  string pwd  = request->pwd();   // "123456"              │   │  ││
│  │    │  │                                                           │   │  ││
│  │    │  │  // 第2步：执行本地业务逻辑                               │   │  ││
│  │    │  │  bool login_result = Login(name, pwd);  // → true         │   │  ││
│  │    │  │                                                           │   │  ││
│  │    │  │  // 第3步：填充 response                                  │   │  ││
│  │    │  │  response->mutable_result()->set_errcode(0);              │   │  ││
│  │    │  │  response->mutable_result()->set_errmsg("");              │   │  ││
│  │    │  │  response->set_success(true);                             │   │  ││
│  │    │  │                                                           │   │  ││
│  │    │  │  // 第4步：触发回调，发回客户端 ★★★                       │   │  ││
│  │    │  │  done->Run();                                            │   │  ││
│  │    │  │     │                                                     │   │  ││
│  │    │  │     │  实际执行：                                         │   │  ││
│  │    │  │     │  MethodClosure2::Run() {                            │   │  ││
│  │    │  │     │    (object_->*method_)(arg1_, arg2_);               │   │  ││
│  │    │  │     │  }                                                  │   │  ││
│  │    │  │     │     = provider指针->SendRpcResponse(conn, response) │   │  ││
│  │    │  │     │     = 序列化 + 打包 + conn->send() 发回客户端       │   │  ││
│  │    │  │     │                                                    │   │  ││
│  │    │  │     ▼                                                    │   │  ││
│  │    │  │  ┌─ KrpcProvider::SendRpcResponse(conn, response) ─┐     │   │  ││
│  │    │  │  │  Krpcprovider.cc:207                              │     │   │  ││
│  │    │  │  │                                                   │     │   │  ││
│  │    │  │  │  ① response->SerializeToString(&response_str)     │     │   │  ││
│  │    │  │  │     → LoginResponse 序列化为字节串                 │     │   │  ││
│  │    │  │  │                                                   │     │   │  ││
│  │    │  │  │  ② 打包：[4B net_len] + [response_str]            │     │   │  ││
│  │    │  │  │     ┌──────────┬──────────────────────┐           │     │   │  ││
│  │    │  │  │     │TotalLen  │ LoginResponse 序列化 │           │     │   │  ││
│  │    │  │  │     │ 4B 网络   │ errcode=0,success=t  │           │     │   │  ││
│  │    │  │  │     └──────────┴──────────────────────┘           │     │   │  ││
│  │    │  │  │                                                   │     │   │  ││
│  │    │  │  │  ③ conn->send(send_buf)  ← muduo 异步发回客户端   │     │   │  ││
│  │    │  │  └───────────────────────────────────────────────────┘     │   │  ││
│  │    │  └────────────────────────────────────────────────────────────┘   │  ││
│  │    └───────────────────────────────────────────────────────────────────┘  ││
│  └──────────────────────────────────────────────────────────────────────────┘│
│                                                                              │
│                        ║  TCP 网络  ║                                       │
│                        ║            ║                                       │
└────────────────────────╬────────────╬───────────────────────────────────────┘
                         ║            ║
┌────────────────────────╬────────────╬───────────────────────────────────────┐
│                        ║  回到客户端  ║                                      │
│                        ║            ║                                      │
│  recv_exact(fd) 收到响应 → 反序列化                                         │
│  response.result().errcode() == 0  → 成功 ✓                                │
│  response.success() == true         → 登录成功 ✓                            │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 为什么会有回调？三条线串起来

```
  OnMessage（框架层）                  NewCallback（粘合层）          业务层
  ──────────────                      ────────────────          ──────
                                                                     
  ① 反射创建 request                  request ────────────►   读 request
     = new LoginRequest()                                      执行业务
                                                                     
  ② 反射创建 response                 response ◄────────────   写 response  
     = new LoginResponse()                                     (空对象被填充)
                                                                     
  ③ 打包回调闭包：                    done ────────────────►   存起来
     NewCallback(                                              │
       this,                                                   │
       &SendRpcResponse,                                       ▼
       conn,                                             done->Run()  ← 业务代码
       response)                                          只这一行，触发回包
     → new MethodClosure2                                    │
       内部 Run() = SendRpcResponse(conn, response)  ◄───────┘
                                                             
  ④ service->CallMethod(...)                                  因为 done 里存的
     传入 done ──────────────────────► 经过 protobuf 分派 →  response 指针
                                       → 业务层 Login()      和业务层填的是
                                                             同一个对象！
                                       
  业务层 done->Run() 触发：
    → SendRpcResponse(conn, 已经填好的 response)
    → 序列化 → conn->send() → 发回客户端
```

**核心**：`done` 里存的 `response` 指针和业务层 `UserService::Login` 收到的 `response` 是**同一个对象**。业务代码往 response 里写数据，写完后调 `done->Run()`，Run() 内部拿着这个已经填好的 response 序列化发走。业务代码完全不用知道网络怎么发包。
