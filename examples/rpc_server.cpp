// 示例 RPC 服务器
#include "RpcServer.h"
#include <iostream>

// 用户服务示例
class UserService {
public:
    void registerMethods(RpcServer* server) {
        server->registerMethod("user.get", [this](const json& params) {
            return getUser(params);
        });
        
        server->registerMethod("user.list", [this](const json& params) {
            return listUsers(params);
        });
        
        server->registerMethod("user.create", [this](const json& params) {
            return createUser(params);
        });
    }
    
private:
    json getUser(const json& params) {
        int id = params.value("id", 0);
        
        // 模拟数据库查询
        json user;
        user["id"] = id;
        user["name"] = "User " + std::to_string(id);
        user["email"] = "user" + std::to_string(id) + "@example.com";
        
        return user;
    }
    
    json listUsers(const json& params) {
        json users = json::array();
        for (int i = 1; i <= 5; ++i) {
            json user;
            user["id"] = i;
            user["name"] = "User " + std::to_string(i);
            users.push_back(user);
        }
        return users;
    }
    
    json createUser(const json& params) {
        json result;
        result["success"] = true;
        result["id"] = 123;  // 新用户 ID
        result["message"] = "User created successfully";
        return result;
    }
};

// 计算器服务示例
class CalculatorService {
public:
    void registerMethods(RpcServer* server) {
        server->registerMethod("calc.add", [this](const json& params) {
            return add(params);
        });
        
        server->registerMethod("calc.sub", [this](const json& params) {
            return sub(params);
        });
        
        server->registerMethod("calc.mul", [this](const json& params) {
            return mul(params);
        });
        
        server->registerMethod("calc.div", [this](const json& params) {
            return div(params);
        });
    }
    
private:
    json add(const json& params) {
        double a = params.value("a", 0.0);
        double b = params.value("b", 0.0);
        return {{"result", a + b}};
    }
    
    json sub(const json& params) {
        double a = params.value("a", 0.0);
        double b = params.value("b", 0.0);
        return {{"result", a - b}};
    }
    
    json mul(const json& params) {
        double a = params.value("a", 0.0);
        double b = params.value("b", 0.0);
        return {{"result", a * b}};
    }
    
    json div(const json& params) {
        double a = params.value("a", 0.0);
        double b = params.value("b", 1.0);
        if (b == 0) {
            return {{"error", "division by zero"}};
        }
        return {{"result", a / b}};
    }
};

int main() {
    EventLoop loop;
    InetAddress addr(8081);
    RpcServer server(&loop, addr);
    
    // 注册服务
    UserService userService;
    userService.registerMethods(&server);
    
    CalculatorService calcService;
    calcService.registerMethods(&server);
    
    std::cout << "RPC Server running on http://localhost:8081/rpc" << std::endl;
    std::cout << "\nAvailable methods:" << std::endl;
    std::cout << "  user.get    - {\"id\": 1}" << std::endl;
    std::cout << "  user.list   - {}" << std::endl;
    std::cout << "  user.create - {\"name\": \"test\", \"email\": \"test@example.com\"}" << std::endl;
    std::cout << "  calc.add    - {\"a\": 1, \"b\": 2}" << std::endl;
    std::cout << "  calc.sub    - {\"a\": 5, \"b\": 3}" << std::endl;
    std::cout << "  calc.mul    - {\"a\": 4, \"b\": 5}" << std::endl;
    std::cout << "  calc.div    - {\"a\": 10, \"b\": 2}" << std::endl;
    
    server.start();
    loop.loop();
    
    return 0;
}