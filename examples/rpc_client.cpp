// RPC 客户端测试
#include "RpcClient.h"
#include <iostream>

int main() {
    RpcClient client("127.0.0.1", 8081);
    
    std::cout << "=== RPC Client Test ===\n" << std::endl;
    
    // 测试计算器服务
    std::cout << "1. calc.add(10, 5)" << std::endl;
    json result = client.call("calc.add", {{"a", 10}, {"b", 5}});
    std::cout << "   Result: " << result.dump() << std::endl;
    
    std::cout << "\n2. calc.mul(6, 7)" << std::endl;
    result = client.call("calc.mul", {{"a", 6}, {"b", 7}});
    std::cout << "   Result: " << result.dump() << std::endl;
    
    // 测试用户服务
    std::cout << "\n3. user.get(id=1)" << std::endl;
    result = client.call("user.get", {{"id", 1}});
    std::cout << "   Result: " << result.dump() << std::endl;
    
    std::cout << "\n4. user.list()" << std::endl;
    result = client.call("user.list", json::object());
    std::cout << "   Result: " << result.dump() << std::endl;
    
    std::cout << "\n5. user.create(name=test)" << std::endl;
    result = client.call("user.create", {{"name", "test"}, {"email", "test@example.com"}});
    std::cout << "   Result: " << result.dump() << std::endl;
    
    return 0;
}