// Protobuf RPC 服务器示例
#include "RpcServerPb.h"
#include "proto/rpc.pb.h"
#include <iostream>

class UserServicePb {
public:
    void registerMethods(RpcServerPb* server) {
        // user.get
        server->registerMethod<rpc::GetUserRequest, rpc::GetUserResponse>(
            "user", "get",
            [this](const rpc::GetUserRequest& req, rpc::GetUserResponse& resp) {
                getUser(req, resp);
            });
        
        // user.list
        server->registerMethod<rpc::ListUsersRequest, rpc::ListUsersResponse>(
            "user", "list",
            [this](const rpc::ListUsersRequest& req, rpc::ListUsersResponse& resp) {
                listUsers(req, resp);
            });
    }
    
private:
    void getUser(const rpc::GetUserRequest& req, rpc::GetUserResponse& resp) {
        // 模拟数据库查询
        resp.set_id(req.id());
        resp.set_name("User " + std::to_string(req.id()));
        resp.set_email("user" + std::to_string(req.id()) + "@example.com");
    }
    
    void listUsers(const rpc::ListUsersRequest& req, rpc::ListUsersResponse& resp) {
        int limit = req.limit() > 0 ? req.limit() : 5;
        for (int i = 1; i <= limit; ++i) {
            auto* user = resp.add_users();
            user->set_id(i);
            user->set_name("User " + std::to_string(i));
            user->set_email("user" + std::to_string(i) + "@example.com");
        }
    }
};

class CalculatorServicePb {
public:
    void registerMethods(RpcServerPb* server) {
        // calc.add
        server->registerMethod<rpc::CalcRequest, rpc::CalcResponse>(
            "calc", "add",
            [this](const rpc::CalcRequest& req, rpc::CalcResponse& resp) {
                resp.set_result(req.a() + req.b());
            });
        
        // calc.sub
        server->registerMethod<rpc::CalcRequest, rpc::CalcResponse>(
            "calc", "sub",
            [this](const rpc::CalcRequest& req, rpc::CalcResponse& resp) {
                resp.set_result(req.a() - req.b());
            });
        
        // calc.mul
        server->registerMethod<rpc::CalcRequest, rpc::CalcResponse>(
            "calc", "mul",
            [this](const rpc::CalcRequest& req, rpc::CalcResponse& resp) {
                resp.set_result(req.a() * req.b());
            });
        
        // calc.div
        server->registerMethod<rpc::CalcRequest, rpc::CalcResponse>(
            "calc", "div",
            [this](const rpc::CalcRequest& req, rpc::CalcResponse& resp) {
                if (req.b() == 0) {
                    resp.set_result(0);
                } else {
                    resp.set_result(req.a() / req.b());
                }
            });
    }
};

int main() {
    EventLoop loop;
    InetAddress addr(8082);
    RpcServerPb server(&loop, addr);
    
    // 注册服务
    UserServicePb userService;
    userService.registerMethods(&server);
    
    CalculatorServicePb calcService;
    calcService.registerMethods(&server);
    
    std::cout << "Protobuf RPC Server running on port 8082" << std::endl;
    std::cout << "\nAvailable methods:" << std::endl;
    std::cout << "  user.get  - GetUserRequest { id: int }" << std::endl;
    std::cout << "  user.list - ListUsersRequest { limit: int }" << std::endl;
    std::cout << "  calc.add  - CalcRequest { a: double, b: double }" << std::endl;
    std::cout << "  calc.sub  - CalcRequest { a: double, b: double }" << std::endl;
    std::cout << "  calc.mul  - CalcRequest { a: double, b: double }" << std::endl;
    std::cout << "  calc.div  - CalcRequest { a: double, b: double }" << std::endl;
    
    server.start();
    loop.loop();
    
    return 0;
}