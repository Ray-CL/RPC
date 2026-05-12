#ifndef _zookeeperutil_h_
#define _zookeeperutil_h_

#include<semaphore.h>
#include<zookeeper/zookeeper.h>
#include<string>
#include<vector>

//封装的zk客户端
class ZkClient
{
public:
    ZkClient();
    ~ZkClient();
    //zkclient启动连接zkserver
    void Start();
    //在zkserver中创建一个节点，根据指定的path
    void Create(const char* path,const char* data,int datalen,int state=0);
    //根据参数指定的znode节点路径，或者znode节点值
    std::string GetData(const char* path);
    //获取指定节点的子节点列表
    std::vector<std::string> GetChildren(const char* path);
    // 获取子节点并设置 watcher 监听子节点变化
    std::vector<std::string> GetChildrenWithWatcher(const char* path,
        watcher_fn watcher, void* ctx);
private:
    //Zk的客户端句柄
    zhandle_t* m_zhandle;
};
#endif
