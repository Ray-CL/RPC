#ifndef _Krpcchannel_h_
#define _Krpcchannel_h_
// 此类是继承自google::protobuf::RpcChannel
// 目的是为了给客户端进行方法调用的时候，统一接收的
#include <google/protobuf/service.h>
#include "zookeeperutil.h"
#include <vector>
#include <chrono>
#include <memory>
#include <unordered_map>

class KrpcChannel : public google::protobuf::RpcChannel
{
public:
    KrpcChannel(bool connectNow);
    virtual ~KrpcChannel();
    void CallMethod(const ::google::protobuf::MethodDescriptor *method,
                    ::google::protobuf::RpcController *controller,
                    const ::google::protobuf::Message *request,
                    ::google::protobuf::Message *response,
                    ::google::protobuf::Closure *done) override; // override可以验证是否是虚函数
private:
    struct ProviderInfo {
        std::string ip;
        uint16_t port;
        bool alive = true;
        std::chrono::steady_clock::time_point last_failed = std::chrono::steady_clock::time_point::min();
    };

    // 连接池，key = "ip:port"
    std::unordered_map<std::string, int> m_conn_pool;
    std::string service_name;
    std::string m_ip;
    uint16_t m_port;
    std::string method_name;
    int m_idx; // 用来划分服务器ip和port的下标
    std::vector<ProviderInfo> m_providers;
    size_t m_provider_index = 0;
    std::chrono::steady_clock::time_point m_last_refresh_time = std::chrono::steady_clock::time_point::min();
    std::unique_ptr<ZkClient> m_zkclient;

    // watcher 触发的脏标记，下次调用时刷新
    bool m_providers_dirty = true;

    bool newConnect(const char *ip, uint16_t port);
    void EnsureZkClient();
    bool RefreshProviders(ZkClient *zkclient, const std::string &service_name, const std::string &method_name);
    bool SelectProvider(std::string &ip, uint16_t &port);
    void MarkProviderFailed(const std::string &ip, uint16_t port);
    bool ConnectToAvailableProvider(ZkClient *zkclient, const std::string &service_name, const std::string &method_name);
    bool ProviderAlive(const ProviderInfo &provider) const;
    ssize_t recv_exact(int fd, char* buf, size_t size);

    // 连接池操作
    std::string PoolKey(const std::string &ip, uint16_t port) const;
    int GetPooledFd(const std::string &ip, uint16_t port);
    void AddToPool(const std::string &ip, uint16_t port, int fd);
    void RemoveFromPool(const std::string &ip, uint16_t port);

    // ZK 子节点变化 watcher 回调
    static void ChildWatcher(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx);
};
#endif
