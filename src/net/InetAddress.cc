#include"InetAddress.h"
#include<strings.h>
#include<string.h>


/**
 * @brief 构造IPv4地址对象
 * 
 * @param port 主机字节序的端口号
 * @param ip 点分十进制字符串格式的IPv4地址
 * 
 * @note 调用者需确保ip参数为合法IPv4地址格式
 *       内部使用inet_addr进行地址转换
 */
InetAddress::InetAddress(uint16_t port, std::string ip)
{ 
    // 初始化sockaddr_in结构体内存
    bzero(&addr_, sizeof addr_);

    // 设置地址族为IPv4
    addr_.sin_family = AF_INET;

    // 将端口转换为网络字节序并存储
    addr_.sin_port = htons(port);

    // 将IP地址字符串转换为网络字节序的32位二进制整数
    addr_.sin_addr.s_addr = inet_addr(ip.c_str());
}

/**
 * 将IPv4地址结构转换为可读的IP地址字符串。
 * 
 * @return std::string 返回IPv4地址的字符串表示，格式为"x.x.x.x"
 *                    其中x为0-255之间的十进制数字
 */

std::string InetAddress::toIpPort() const
{
    char buf[64];
    /**
     * 使用inet_ntop函数将sockaddr_in结构中的IPv4地址转换为
     * 点分十进制字符串表示。AF_INET指定IPv4协议族，
     * addr_.sin_addr为存储IPv4地址的in_addr结构体，
     * buf用于存储转换后的字符串结果，缓冲区大小为64字节
     */
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof buf);
    size_t end = strlen(buf);
    uint16_t port = ntohs(addr_.sin_port);
    /**
     * 将网络字节序的端口号转换为主机字节序后，
     * 使用sprintf将端口号格式化追加到IP字符串末尾，
     * 形成完整的"ip:port"格式字符串
     */
    snprintf(buf + end, sizeof(buf) - end, ":%u", port);
    return buf;
}

uint16_t InetAddress::toPort() const
{
    return ntohs(addr_.sin_port);
}


// #include<iostream>
// int main()
// {
//     InetAddress addr(8080);
//     std::cout << addr.toIpPort() << std::endl;
//     std::cout << addr.toPort() << std::endl;
//     return 0; 
// }