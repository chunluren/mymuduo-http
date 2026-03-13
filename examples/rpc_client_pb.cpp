// Protobuf RPC 客户端示例
#include "RpcClientPb.h"
#include "proto/rpc.pb.h"
#include <iostream>

int main() {
    RpcClientPb client("127.0.0.1", 8082);
    
    if (!client.connect()) {
        std::cerr << "Failed to connect" << std::endl;
        return 1;
    }
    
    std::cout << "=== Protobuf RPC Client Test ===\n" << std::endl;
    
    // 测试计算器服务
    {
        std::cout << "1. calc.add(10, 5)" << std::endl;
        rpc::CalcRequest req;
        req.set_a(10);
        req.set_b(5);
        
        rpc::CalcResponse resp;
        if (client.call<rpc::CalcRequest, rpc::CalcResponse>("calc", "add", req, resp)) {
            std::cout << "   Result: " << resp.result() << std::endl;
        } else {
            std::cout << "   Failed!" << std::endl;
        }
    }
    
    {
        std::cout << "\n2. calc.mul(6, 7)" << std::endl;
        rpc::CalcRequest req;
        req.set_a(6);
        req.set_b(7);
        
        rpc::CalcResponse resp;
        if (client.call<rpc::CalcRequest, rpc::CalcResponse>("calc", "mul", req, resp)) {
            std::cout << "   Result: " << resp.result() << std::endl;
        }
    }
    
    // 测试用户服务
    {
        std::cout << "\n3. user.get(id=1)" << std::endl;
        rpc::GetUserRequest req;
        req.set_id(1);
        
        rpc::GetUserResponse resp;
        if (client.call<rpc::GetUserRequest, rpc::GetUserResponse>("user", "get", req, resp)) {
            std::cout << "   User: id=" << resp.id() 
                      << ", name=" << resp.name()
                      << ", email=" << resp.email() << std::endl;
        }
    }
    
    {
        std::cout << "\n4. user.list(limit=3)" << std::endl;
        rpc::ListUsersRequest req;
        req.set_limit(3);
        
        rpc::ListUsersResponse resp;
        if (client.call<rpc::ListUsersRequest, rpc::ListUsersResponse>("user", "list", req, resp)) {
            std::cout << "   Users:" << std::endl;
            for (const auto& user : resp.users()) {
                std::cout << "     - id=" << user.id() 
                          << ", name=" << user.name() << std::endl;
            }
        }
    }
    
    // 异步调用测试
    {
        std::cout << "\n5. Async call: calc.sub(20, 8)" << std::endl;
        rpc::CalcRequest req;
        req.set_a(20);
        req.set_b(8);
        
        rpc::CalcResponse resp;
        auto future = client.asyncCall<rpc::CalcRequest, rpc::CalcResponse>(
            "calc", "sub", req, resp);
        
        // 可以做其他事情...
        
        if (future.get()) {
            std::cout << "   Result: " << resp.result() << std::endl;
        }
    }
    
    client.disconnect();
    
    std::cout << "\n=== Test completed ===" << std::endl;
    return 0;
}