#include "Krpcchannel.h"
#include "Krpcheader.pb.h"
#include "zookeeperutil.h"
#include "Krpcapplication.h"
#include "Krpccontroller.h"
#include "memory"
#include <algorithm>
#include <chrono>
#include <errno.h>
#include <mutex>
#include <random>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include "KrpcLogger.h"

std::mutex g_data_mutx;  // 全局互斥锁

// ==================== 连接池操作 ====================

std::string KrpcChannel::PoolKey(const std::string &ip, uint16_t port) const {
    return ip + ":" + std::to_string(port);
}

int KrpcChannel::GetPooledFd(const std::string &ip, uint16_t port) {
    auto it = m_conn_pool.find(PoolKey(ip, port));
    if (it != m_conn_pool.end()) {
        return it->second;
    }
    return -1;
}

void KrpcChannel::AddToPool(const std::string &ip, uint16_t port, int fd) {
    m_conn_pool[PoolKey(ip, port)] = fd;
}

void KrpcChannel::RemoveFromPool(const std::string &ip, uint16_t port) {
    std::string key = PoolKey(ip, port);
    auto it = m_conn_pool.find(key);
    if (it != m_conn_pool.end()) {
        close(it->second);
        m_conn_pool.erase(it);
    }
}

// ==================== ZK Watcher ====================

void KrpcChannel::ChildWatcher(zhandle_t *zh, int type, int state,
                                const char *path, void *watcherCtx) {
    if (type == ZOO_CHILD_EVENT) {
        KrpcChannel *channel = static_cast<KrpcChannel*>(watcherCtx);
        channel->m_providers_dirty = true;
    }
}

// ==================== 网络 IO ====================

ssize_t KrpcChannel::recv_exact(int fd, char* buf, size_t size) {
    size_t total_read = 0;
    while (total_read < size) {
        ssize_t ret = recv(fd, buf + total_read, size - total_read, 0);
        if (ret == 0) return 0;
        if (ret == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        total_read += ret;
    }
    return total_read;
}

// ==================== RPC 调用入口 ====================

void KrpcChannel::CallMethod(const ::google::protobuf::MethodDescriptor *method,
                             ::google::protobuf::RpcController *controller,
                             const ::google::protobuf::Message *request,
                             ::google::protobuf::Message *response,
                             ::google::protobuf::Closure *done)
{
    const google::protobuf::ServiceDescriptor *sd = method->service();
    service_name = sd->name();
    method_name = method->name();

    EnsureZkClient();

    for (int attempt = 0; attempt < 2; ++attempt) {
        // 从连接池获取当前提供者的 fd
        int fd = GetPooledFd(m_ip, m_port);
        if (fd < 0) {
            if (!ConnectToAvailableProvider(m_zkclient.get(), service_name, method_name)) {
                controller->SetFailed("no available service provider");
                return;
            }
            fd = GetPooledFd(m_ip, m_port);
        }

        // 序列化请求参数
        std::string args_str;
        if (!request->SerializeToString(&args_str)) {
            controller->SetFailed("serialize request fail");
            return;
        }

        // 构建协议头
        Krpc::RpcHeader krpcheader;
        krpcheader.set_service_name(service_name);
        krpcheader.set_method_name(method_name);
        krpcheader.set_args_size(args_str.size());

        std::string rpc_header_str;
        if (!krpcheader.SerializeToString(&rpc_header_str)) {
            controller->SetFailed("serialize rpc header error!");
            return;
        }

        // 打包
        uint32_t header_size = rpc_header_str.size();
        uint32_t total_len = 4 + header_size + args_str.size();

        uint32_t net_total_len = htonl(total_len);
        uint32_t net_header_len = htonl(header_size);

        std::string send_rpc_str;
        send_rpc_str.reserve(4 + 4 + header_size + args_str.size());
        send_rpc_str.append((char*)&net_total_len, 4);
        send_rpc_str.append((char*)&net_header_len, 4);
        send_rpc_str.append(rpc_header_str);
        send_rpc_str.append(args_str);

        // 发送
        if (-1 == send(fd, send_rpc_str.c_str(), send_rpc_str.size(), 0)) {
            MarkProviderFailed(m_ip, m_port);
            RemoveFromPool(m_ip, m_port);
            continue;
        }

        // 接收响应长度
        uint32_t response_len = 0;
        if (recv_exact(fd, (char*)&response_len, 4) != 4) {
            MarkProviderFailed(m_ip, m_port);
            RemoveFromPool(m_ip, m_port);
            continue;
        }
        response_len = ntohl(response_len);

        // 接收响应体
        std::vector<char> recv_buf(response_len);
        if (recv_exact(fd, recv_buf.data(), response_len) != response_len) {
            MarkProviderFailed(m_ip, m_port);
            RemoveFromPool(m_ip, m_port);
            continue;
        }

        // 反序列化响应
        if (!response->ParseFromArray(recv_buf.data(), response_len)) {
            MarkProviderFailed(m_ip, m_port);
            RemoveFromPool(m_ip, m_port);
            continue;
        }

        // 多实例：从池中移除（下次调用重新选节点，实现负载均衡）
        if (m_providers.size() > 1) {
            RemoveFromPool(m_ip, m_port);
        }
        return;
    }

    controller->SetFailed("rpc call failed after retrying providers");
    return;
}

// ==================== 连接管理 ====================

bool KrpcChannel::newConnect(const char *ip, uint16_t port) {
    int clientfd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == clientfd) {
        char errtxt[512] = {0};
        LOG(ERROR) << "socket error:" << strerror_r(errno, errtxt, sizeof(errtxt));
        return false;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);

    if (-1 == connect(clientfd, (struct sockaddr *)&server_addr, sizeof(server_addr))) {
        close(clientfd);
        char errtxt[512] = {0};
        LOG(ERROR) << "connect error:" << strerror_r(errno, errtxt, sizeof(errtxt));
        return false;
    }

    // 连上后放入连接池
    AddToPool(ip, port, clientfd);
    return true;
}

// ==================== 故障隔离 ====================

bool KrpcChannel::ProviderAlive(const ProviderInfo &provider) const {
    if (provider.alive) {
        return true;
    }
    auto now = std::chrono::steady_clock::now();
    return now - provider.last_failed >= std::chrono::seconds(3);
}

void KrpcChannel::MarkProviderFailed(const std::string &ip, uint16_t port) {
    for (auto &provider : m_providers) {
        if (provider.ip == ip && provider.port == port) {
            provider.alive = false;
            provider.last_failed = std::chrono::steady_clock::now();
            break;
        }
    }
}

// ==================== 服务发现 ====================

bool KrpcChannel::RefreshProviders(ZkClient *zkclient, const std::string &service_name,
                                    const std::string &method_name) {
    std::string method_path = "/" + service_name + "/" + method_name;
    std::vector<std::string> children;

    {
        std::unique_lock<std::mutex> lock(g_data_mutx);
        // 使用带 watcher 的 GetChildren：获取子节点并注册变化监听
        children = zkclient->GetChildrenWithWatcher(method_path.c_str(),
            &KrpcChannel::ChildWatcher, this);
    }

    if (children.empty()) {
        return false;
    }

    std::vector<ProviderInfo> next_providers;
    for (auto &child : children) {
        std::string child_path = method_path + "/" + child;
        std::string host_data;
        {
            std::unique_lock<std::mutex> lock(g_data_mutx);
            host_data = zkclient->GetData(child_path.c_str());
        }
        if (host_data.empty()) {
            continue;
        }
        int pos = host_data.find(":");
        if (pos == -1) {
            continue;
        }
        ProviderInfo provider;
        provider.ip = host_data.substr(0, pos);
        provider.port = atoi(host_data.substr(pos + 1).c_str());
        // 继承旧节点的故障状态
        for (auto &old : m_providers) {
            if (old.ip == provider.ip && old.port == provider.port) {
                provider.alive = old.alive;
                provider.last_failed = old.last_failed;
                break;
            }
        }
        next_providers.emplace_back(std::move(provider));
    }

    if (next_providers.empty()) {
        return false;
    }

    m_providers = std::move(next_providers);
    m_last_refresh_time = std::chrono::steady_clock::now();
    m_providers_dirty = false;  // 刷新完成，清除脏标记
    if (m_provider_index >= m_providers.size()) {
        m_provider_index = 0;
    }
    return true;
}

bool KrpcChannel::SelectProvider(std::string &ip, uint16_t &port) {
    if (m_providers.empty()) {
        return false;
    }

    std::vector<size_t> alive_indices;
    for (size_t i = 0; i < m_providers.size(); ++i) {
        if (ProviderAlive(m_providers[i])) {
            alive_indices.push_back(i);
        }
    }
    if (alive_indices.empty()) {
        return false;
    }

    static thread_local std::mt19937 gen((std::random_device())());
    std::uniform_int_distribution<size_t> dist(0, alive_indices.size() - 1);
    size_t chosen_index = alive_indices[dist(gen)];
    ip = m_providers[chosen_index].ip;
    port = m_providers[chosen_index].port;
    m_provider_index = (chosen_index + 1) % m_providers.size();
    return true;
}

void KrpcChannel::EnsureZkClient() {
    if (!m_zkclient) {
        m_zkclient.reset(new ZkClient());
        m_zkclient->Start();
    }
}

bool KrpcChannel::ConnectToAvailableProvider(ZkClient *zkclient,
                                              const std::string &service_name,
                                              const std::string &method_name) {
    auto now = std::chrono::steady_clock::now();
    // 刷新条件：watcher 触发脏标记 || 列表为空 || 超过 10 秒未刷新（兜底）
    if (m_providers_dirty || m_providers.empty() ||
        now - m_last_refresh_time > std::chrono::seconds(10)) {
        if (!RefreshProviders(zkclient, service_name, method_name)) {
            return false;
        }
    }

    std::string ip;
    uint16_t port;
    // 第一轮：尝试存活节点（优先从连接池复用）
    while (SelectProvider(ip, port)) {
        int pooled = GetPooledFd(ip, port);
        if (pooled >= 0) {
            m_ip = ip;
            m_port = port;
            return true;
        }
        if (newConnect(ip.c_str(), port)) {
            m_ip = ip;
            m_port = port;
            return true;
        }
        MarkProviderFailed(ip, port);
    }

    // 第二轮：ZK 再次刷新兜底
    if (RefreshProviders(zkclient, service_name, method_name)) {
        while (SelectProvider(ip, port)) {
            int pooled = GetPooledFd(ip, port);
            if (pooled >= 0) {
                m_ip = ip;
                m_port = port;
                return true;
            }
            if (newConnect(ip.c_str(), port)) {
                m_ip = ip;
                m_port = port;
                return true;
            }
            MarkProviderFailed(ip, port);
        }
    }

    return false;
}

// ==================== 构造/析构 ====================

KrpcChannel::KrpcChannel(bool connectNow) : m_idx(0) {
    if (!connectNow) {
        return;
    }
    // connectNow=true 时立即连接（通常不使用此路径）
    auto rt = newConnect(m_ip.c_str(), m_port);
    int count = 3;
    while (!rt && count--) {
        rt = newConnect(m_ip.c_str(), m_port);
    }
}

KrpcChannel::~KrpcChannel() {
    for (auto &kv : m_conn_pool) {
        if (kv.second >= 0) {
            close(kv.second);
        }
    }
    m_conn_pool.clear();
}
