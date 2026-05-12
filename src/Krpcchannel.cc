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

std::mutex g_data_mutx;  // 全局互斥锁，用于保护共享数据的线程安全


// 辅助函数：循环读取直到读够 size 字节
ssize_t KrpcChannel::recv_exact(int fd, char* buf, size_t size) {
    size_t total_read = 0;
    while (total_read < size) {
        ssize_t ret = recv(fd, buf + total_read, size - total_read, 0);
        if (ret == 0) return 0; // 对端关闭
        if (ret == -1) {
            if (errno == EINTR) continue; // 中断信号，继续读
            return -1; // 错误
        }
        total_read += ret;
    }
    return total_read;
}

// RPC调用的核心方法，负责将客户端的请求序列化并发送到服务端，同时接收服务端的响应
void KrpcChannel::CallMethod(const ::google::protobuf::MethodDescriptor *method,
                             ::google::protobuf::RpcController *controller,
                             const ::google::protobuf::Message *request,
                             ::google::protobuf::Message *response,
                             ::google::protobuf::Closure *done)
{
    // 获取服务对象名和方法名
    const google::protobuf::ServiceDescriptor *sd = method->service();
    service_name = sd->name();  // 服务名
    method_name = method->name();  // 方法名

    // 客户端需要查询ZooKeeper，找到提供该服务的服务器地址
    EnsureZkClient();

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (-1 == m_clientfd) {
            if (!ConnectToAvailableProvider(m_zkclient.get(), service_name, method_name)) {
                controller->SetFailed("no available service provider");
                return;
            }
            LOG(INFO) << "connect server success";  // 连接成功，记录日志
        }

        // 2. 序列化请求参数
        std::string args_str;
        if (!request->SerializeToString(&args_str)) {
            controller->SetFailed("serialize request fail");
            return;
        }

        // 3. 构建协议头
        Krpc::RpcHeader krpcheader;
        krpcheader.set_service_name(service_name);
        krpcheader.set_method_name(method_name);
        krpcheader.set_args_size(args_str.size());

        std::string rpc_header_str;
        if (!krpcheader.SerializeToString(&rpc_header_str)) {
            controller->SetFailed("serialize rpc header error!");
            return;
        }

        // 4. 打包数据发送
        // 格式：[4B Total Len] + [4B Header Len] + [Header] + [Args]

        uint32_t header_size = rpc_header_str.size();
        uint32_t total_len = 4 + header_size + args_str.size(); // Total Len 包含 HeaderLen(4) + Header + Body

        // 转网络字节序
        uint32_t net_total_len = htonl(total_len);
        uint32_t net_header_len = htonl(header_size);

        std::string send_rpc_str;
        send_rpc_str.reserve(4 + 4 + header_size + args_str.size());
        
        send_rpc_str.append((char*)&net_total_len, 4);
        send_rpc_str.append((char*)&net_header_len, 4);
        send_rpc_str.append(rpc_header_str);
        send_rpc_str.append(args_str);

        // 发送
        if (-1 == send(m_clientfd, send_rpc_str.c_str(), send_rpc_str.size(), 0)) {
            MarkProviderFailed(m_ip, m_port);
            close(m_clientfd);
            m_clientfd = -1; // 重置
            continue;
        }

        // 5. 接收响应
        // 格式：[4B Total Len] + [Response Data]
        
        // A. 先读4字节长度头
        uint32_t response_len = 0;
        if (recv_exact(m_clientfd, (char*)&response_len, 4) != 4) {
            MarkProviderFailed(m_ip, m_port);
            close(m_clientfd);
            m_clientfd = -1;
            continue;
        }
        response_len = ntohl(response_len); // 转回主机字节序

        // B. 根据长度读取Body
        std::vector<char> recv_buf(response_len);
        if (recv_exact(m_clientfd, recv_buf.data(), response_len) != response_len) {
            MarkProviderFailed(m_ip, m_port);
            close(m_clientfd);
            m_clientfd = -1;
            continue;
        }

        // 6. 反序列化响应
        if (!response->ParseFromArray(recv_buf.data(), response_len)) {
            MarkProviderFailed(m_ip, m_port);
            close(m_clientfd);
            m_clientfd = -1;
            continue;
        }

        if (m_providers.size() > 1) {
            close(m_clientfd);
            m_clientfd = -1;
        }
        return;
    }

    controller->SetFailed("rpc call failed after retrying providers");
    return;
}

// 创建新的socket连接
bool KrpcChannel::newConnect(const char *ip, uint16_t port) {
    // 创建socket
    int clientfd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == clientfd) {
        char errtxt[512] = {0};
        std::cout << "socket error" << strerror_r(errno, errtxt, sizeof(errtxt)) << std::endl;  // 打印错误信息
        LOG(ERROR) << "socket error:" << errtxt;  // 记录错误日志
        return false;
    }

    // 设置服务器地址信息
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;  // IPv4地址族
    server_addr.sin_port = htons(port);  // 端口号
    server_addr.sin_addr.s_addr = inet_addr(ip);  // IP地址

    // 尝试连接服务器
    if (-1 == connect(clientfd, (struct sockaddr *)&server_addr, sizeof(server_addr))) {
        close(clientfd);  // 连接失败，关闭socket
        char errtxt[512] = {0};
        std::cout << "connect error" << strerror_r(errno, errtxt, sizeof(errtxt)) << std::endl;  // 打印错误信息
        LOG(ERROR) << "connect server error" << errtxt;  // 记录错误日志
        return false;
    }

    m_clientfd = clientfd;  // 保存socket文件描述符
    return true;
}

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

bool KrpcChannel::RefreshProviders(ZkClient *zkclient, const std::string &service_name, const std::string &method_name) {
    std::string method_path = "/" + service_name + "/" + method_name;
    std::vector<std::string> children;

    {
        std::unique_lock<std::mutex> lock(g_data_mutx);
        children = zkclient->GetChildren(method_path.c_str());
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
    if (m_provider_index >= m_providers.size()) {
        m_provider_index = 0;
    }
    return true;
}
//随机选择一个存活或者经过故障隔离的节点
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

bool KrpcChannel::ConnectToAvailableProvider(ZkClient *zkclient, const std::string &service_name, const std::string &method_name) {
    auto now = std::chrono::steady_clock::now();
    if (m_providers.empty() || now - m_last_refresh_time > std::chrono::seconds(10)) {
        if (!RefreshProviders(zkclient, service_name, method_name)) {
            return false;
        }
    }

    std::string ip;
    uint16_t port;
    while (SelectProvider(ip, port)) {
        if (newConnect(ip.c_str(), port)) {
            m_ip = ip;
            m_port = port;
            return true;
        }
        MarkProviderFailed(ip, port);
    }

    // 再刷新一次列表，防止旧列表全部被标记失败
    if (RefreshProviders(zkclient, service_name, method_name)) {
        while (SelectProvider(ip, port)) {
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


// 构造函数，支持延迟连接
KrpcChannel::KrpcChannel(bool connectNow) : m_clientfd(-1), m_idx(0) {
    if (!connectNow) {  // 如果不需要立即连接
        return;
    }

    // 尝试连接服务器，最多重试3次
    auto rt = newConnect(m_ip.c_str(), m_port);
    int count = 3;  // 重试次数
    while (!rt && count--) {
        rt = newConnect(m_ip.c_str(), m_port);
    }

}