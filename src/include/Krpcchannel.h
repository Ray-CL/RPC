#ifndef _Krpcchannel_h_
#define _Krpcchannel_h_
// 此类是继承自google::protobuf::RpcChannel
// 目的是为了给客户端进行方法调用的时候，统一接收的
#include <google/protobuf/service.h>
#include "zookeeperutil.h"
#include <vector>
#include <chrono>
#include <memory>

class KrpcChannel : public google::protobuf::RpcChannel
{
public:
    KrpcChannel(bool connectNow);
    virtual ~KrpcChannel()
    {
          if (m_clientfd >= 0) {
        close(m_clientfd);
    }  
    }
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

    int m_clientfd; // 存放客户端套接字
    std::string service_name;
    std::string m_ip;
    uint16_t m_port;
    std::string method_name;
    int m_idx; // 用来划分服务器ip和port的下标
    std::vector<ProviderInfo> m_providers;
    size_t m_provider_index = 0;
    std::chrono::steady_clock::time_point m_last_refresh_time = std::chrono::steady_clock::time_point::min();
    std::unique_ptr<ZkClient> m_zkclient;

    bool newConnect(const char *ip, uint16_t port);
    void EnsureZkClient();
    bool RefreshProviders(ZkClient *zkclient, const std::string &service_name, const std::string &method_name);
    bool SelectProvider(std::string &ip, uint16_t &port);
    void MarkProviderFailed(const std::string &ip, uint16_t port);
    bool ConnectToAvailableProvider(ZkClient *zkclient, const std::string &service_name, const std::string &method_name);
    bool ProviderAlive(const ProviderInfo &provider) const;
       // 新增：确保读取指定长度的数据，解决TCP拆包
    ssize_t recv_exact(int fd, char* buf, size_t size);
};
#endif
