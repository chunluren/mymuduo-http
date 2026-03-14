// benchmark_client.cpp - 压测客户端
// 用于 HTTP 和 RPC 性能测试

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

// 统计结果
struct Stats {
    int total_requests = 0;
    int successful = 0;
    int failed = 0;
    double total_time_ms = 0;
    std::vector<double> latencies;
    std::mutex mutex;

    void addResult(bool success, double latency_ms) {
        std::lock_guard<std::mutex> lock(mutex);
        total_requests++;
        if (success) {
            successful++;
            latencies.push_back(latency_ms);
        } else {
            failed++;
        }
    }
};

// TCP 客户端
class TcpClient {
public:
    TcpClient(const std::string& host, int port)
        : host_(host), port_(port), fd_(-1) {}

    ~TcpClient() {
        close();
    }

    bool connect() {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);

        if (::connect(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        return true;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool send(const std::string& data) {
        return ::send(fd_, data.c_str(), data.size(), MSG_NOSIGNAL) == (ssize_t)data.size();
    }

    bool recv(std::string& response, size_t max_size = 4096) {
        char buf[4096];
        ssize_t n = ::recv(fd_, buf, std::min(max_size, sizeof(buf)), 0);
        if (n <= 0) return false;
        response.assign(buf, n);
        return true;
    }

private:
    std::string host_;
    int port_;
    int fd_;
};

// HTTP 请求
std::string makeHttpRequest(const std::string& host, int port, const std::string& path,
                            const std::string& method = "GET", const std::string& body = "") {
    std::string req;
    req += method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    req += "Connection: keep-alive\r\n";
    if (!body.empty()) {
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    if (!body.empty()) {
        req += body;
    }
    return req;
}

// JSON-RPC 请求
std::string makeJsonRpcRequest(const std::string& method, const std::string& params, int id = 1) {
    std::string req = R"({"jsonrpc":"2.0","method":")" + method + R"(","params":)" + params + R"(,"id":)" + std::to_string(id) + "}";
    return req;
}

// 解析 HTTP 响应状态码
int parseHttpStatus(const std::string& response) {
    auto pos = response.find("HTTP/1.1 ");
    if (pos == std::string::npos) return 0;
    pos += 9;
    return std::stoi(response.substr(pos, 3));
}

// 计算百分位数
double percentile(std::vector<double>& data, double p) {
    if (data.empty()) return 0;
    std::sort(data.begin(), data.end());
    size_t idx = static_cast<size_t>(data.size() * p / 100.0);
    return data[std::min(idx, data.size() - 1)];
}

// 打印统计
void printStats(const Stats& stats, double total_time_s) {
    auto& latencies = const_cast<std::vector<double>&>(stats.latencies);
    std::sort(latencies.begin(), latencies.end());

    double avg = latencies.empty() ? 0 : std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    double p50 = percentile(latencies, 50);
    double p90 = percentile(latencies, 90);
    double p99 = percentile(latencies, 99);
    double qps = stats.total_requests / total_time_s;

    std::cout << "\n========================================\n";
    std::cout << "性能测试报告\n";
    std::cout << "========================================\n";
    std::cout << "总请求数:       " << stats.total_requests << "\n";
    std::cout << "成功请求:       " << stats.successful << "\n";
    std::cout << "失败请求:       " << stats.failed << "\n";
    std::cout << "总耗时:         " << total_time_s << "s\n";
    std::cout << "----------------------------------------\n";
    std::cout << "平均延迟:       " << avg << "ms\n";
    std::cout << "P50 延迟:       " << p50 << "ms\n";
    std::cout << "P90 延迟:       " << p90 << "ms\n";
    std::cout << "P99 延迟:       " << p99 << "ms\n";
    std::cout << "----------------------------------------\n";
    std::cout << "QPS:            " << qps << " req/s\n";
    std::cout << "========================================\n";
}

// HTTP 压测
void httpBenchmark(const std::string& host, int port, int threads, int requests_per_thread) {
    Stats stats;
    std::vector<std::thread> workers;

    auto worker = [&](int thread_id) {
        TcpClient client(host, port);
        if (!client.connect()) {
            stats.addResult(false, 0);
            return;
        }

        for (int i = 0; i < requests_per_thread; i++) {
            auto start = std::chrono::high_resolution_clock::now();

            std::string req = makeHttpRequest(host, port, "/api/hello");
            if (!client.send(req)) {
                stats.addResult(false, 0);
                continue;
            }

            std::string resp;
            if (!client.recv(resp)) {
                stats.addResult(false, 0);
                continue;
            }

            auto end = std::chrono::high_resolution_clock::now();
            double latency = std::chrono::duration<double, std::milli>(end - start).count();

            int status = parseHttpStatus(resp);
            stats.addResult(status == 200, latency);
        }

        client.close();
    };

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < threads; i++) {
        workers.emplace_back(worker, i);
    }

    for (auto& w : workers) {
        w.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_time = std::chrono::duration<double>(end_time - start_time).count();

    printStats(stats, total_time);
}

// JSON-RPC 压测
void rpcBenchmark(const std::string& host, int port, int threads, int requests_per_thread) {
    Stats stats;
    std::vector<std::thread> workers;

    auto worker = [&](int thread_id) {
        TcpClient client(host, port);
        if (!client.connect()) {
            stats.addResult(false, 0);
            return;
        }

        for (int i = 0; i < requests_per_thread; i++) {
            auto start = std::chrono::high_resolution_clock::now();

            std::string body = makeJsonRpcRequest("calc.add", R"({"a":10,"b":20})");
            std::string req = makeHttpRequest(host, port, "/rpc", "POST", body);

            if (!client.send(req)) {
                stats.addResult(false, 0);
                continue;
            }

            std::string resp;
            if (!client.recv(resp)) {
                stats.addResult(false, 0);
                continue;
            }

            auto end = std::chrono::high_resolution_clock::now();
            double latency = std::chrono::duration<double, std::milli>(end - start).count();

            int status = parseHttpStatus(resp);
            stats.addResult(status == 200, latency);
        }

        client.close();
    };

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < threads; i++) {
        workers.emplace_back(worker, i);
    }

    for (auto& w : workers) {
        w.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_time = std::chrono::duration<double>(end_time - start_time).count();

    printStats(stats, total_time);
}

void printUsage(const char* prog) {
    std::cout << "用法: " << prog << " [选项]\n";
    std::cout << "\n选项:\n";
    std::cout << "  --type <http|rpc>    测试类型 (默认: http)\n";
    std::cout << "  --host <host>        服务器地址 (默认: 127.0.0.1)\n";
    std::cout << "  --port <port>        服务器端口 (默认: 8080)\n";
    std::cout << "  --threads <n>        并发线程数 (默认: 4)\n";
    std::cout << "  --requests <n>       每线程请求数 (默认: 1000)\n";
    std::cout << "\n示例:\n";
    std::cout << "  " << prog << " --type http --port 8080 --threads 8 --requests 1000\n";
    std::cout << "  " << prog << " --type rpc --port 8081 --threads 4 --requests 500\n";
}

int main(int argc, char* argv[]) {
    std::string type = "http";
    std::string host = "127.0.0.1";
    int port = 8080;
    int threads = 4;
    int requests = 1000;

    // 解析参数
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--type" && i + 1 < argc) {
            type = argv[++i];
        } else if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = std::stoi(argv[++i]);
        } else if (arg == "--requests" && i + 1 < argc) {
            requests = std::stoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
    }

    std::cout << "========================================\n";
    std::cout << "mymuduo-http 压测客户端\n";
    std::cout << "========================================\n";
    std::cout << "测试类型: " << type << "\n";
    std::cout << "目标地址: " << host << ":" << port << "\n";
    std::cout << "并发线程: " << threads << "\n";
    std::cout << "每线程请求: " << requests << "\n";
    std::cout << "总请求数: " << threads * requests << "\n";
    std::cout << "========================================\n\n";

    if (type == "http") {
        httpBenchmark(host, port, threads, requests);
    } else if (type == "rpc") {
        rpcBenchmark(host, port, threads, requests);
    } else {
        std::cerr << "未知测试类型: " << type << "\n";
        return 1;
    }

    return 0;
}