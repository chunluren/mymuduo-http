/**
 * @file EtcdClient.h
 * @brief 同步 etcd v3 HTTP gateway 客户端（Phase 3：服务发现替代）
 *
 * 走 etcd v3beta（3.3）/ v3（3.4+）的 HTTP gateway，所有请求都是 POST + JSON。
 * 只实现 IM 项目用得到的 4 个动作：
 *
 *   - put(key, value, leaseId=0)
 *   - getPrefix(prefix) → list of (key, value)
 *   - grantLease(ttlSec) → leaseId
 *   - keepAlive(leaseId) → ttl_remaining
 *   - delete(key)
 *
 * Watch 没实现 —— etcd v3 watch 是 bidi stream，HTTP gateway 也是 long-poll
 * stream，对同步客户端不友好。Phase 3 用 **每 1-2 秒 getPrefix 轮询** 替代，
 * 工程上简单可靠。生产想要真 watch 推荐换 etcd-cpp-apiv3 或基于 grpc++ 自己接。
 *
 * **不依赖 mymuduo 的 reactor**：用裸 socket 同步收发，适合在工作线程或独立
 * 注册线程里调。每次调用建立一个短连接，~ms 量级，对启动/续期场景足够。
 *
 * 使用：
 * @code
 *   EtcdClient etcd("127.0.0.1", 2379);
 *   auto leaseId = etcd.grantLease(15);
 *   etcd.put("services/logic/A", R"({"addr":"127.0.0.1:9100"})", leaseId);
 *   // 每 5s
 *   etcd.keepAlive(leaseId);
 *   // 列出
 *   auto kvs = etcd.getPrefix("services/logic/");
 * @endcode
 *
 * etcd 路径：旧版 v3.0-3.3 用 `/v3beta/...`，新版 v3.4+ 用 `/v3/...`。
 * 构造时可传 apiPrefix，默认 "/v3beta" 兼容 ubuntu 22.04 自带的 3.3.25。
 */
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

class EtcdClient {
public:
    EtcdClient(std::string host, int port, std::string apiPrefix = "/v3beta")
        : host_(std::move(host)), port_(port), apiPrefix_(std::move(apiPrefix)) {}

    /**
     * @brief 申请一个 TTL lease
     * @return leaseId（>0 成功；0 失败）
     */
    int64_t grantLease(int ttlSec) {
        nlohmann::json req = { {"TTL", ttlSec} };
        auto resp = post(apiPrefix_ + "/lease/grant", req.dump());
        if (resp.empty()) return 0;
        auto j = nlohmann::json::parse(resp, nullptr, false);
        if (j.is_discarded()) return 0;
        if (!j.contains("ID")) return 0;
        // etcd 返回的 ID 是字符串形式的整数
        try {
            return std::stoll(j["ID"].is_string() ? j["ID"].get<std::string>()
                                                   : std::to_string(j["ID"].get<int64_t>()));
        } catch (...) { return 0; }
    }

    /// 续期 lease；返回剩余 ttl（0 = 失败）
    int keepAlive(int64_t leaseId) {
        nlohmann::json req = { {"ID", std::to_string(leaseId)} };
        auto resp = post(apiPrefix_ + "/lease/keepalive", req.dump());
        if (resp.empty()) return 0;
        auto j = nlohmann::json::parse(resp, nullptr, false);
        if (j.is_discarded()) return 0;
        if (!j.contains("result") || !j["result"].contains("TTL")) return 0;
        try {
            auto& t = j["result"]["TTL"];
            return std::stoi(t.is_string() ? t.get<std::string>()
                                            : std::to_string(t.get<int>()));
        } catch (...) { return 0; }
    }

    /// PUT key=value，可选 leaseId（0 = 无 lease 永久）
    bool put(const std::string& key, const std::string& value, int64_t leaseId = 0) {
        nlohmann::json req = {
            {"key",   base64(key)},
            {"value", base64(value)},
        };
        if (leaseId > 0) req["lease"] = std::to_string(leaseId);
        auto resp = post(apiPrefix_ + "/kv/put", req.dump());
        return !resp.empty();
    }

    /// 删 key
    bool del(const std::string& key) {
        nlohmann::json req = { {"key", base64(key)} };
        auto resp = post(apiPrefix_ + "/kv/deleterange", req.dump());
        return !resp.empty();
    }

    struct KV { std::string key; std::string value; };

    /// 按 prefix 列出所有 key/value（解码后）
    std::vector<KV> getPrefix(const std::string& prefix) {
        // range_end = prefix 字节加 1 → 覆盖整个前缀范围
        std::string rangeEnd = prefix;
        if (!rangeEnd.empty()) {
            // increment 最后一个字节；若是 0xFF 则补一个 0x00
            unsigned char last = static_cast<unsigned char>(rangeEnd.back());
            if (last == 0xFF) rangeEnd.push_back('\x00');
            else rangeEnd.back() = static_cast<char>(last + 1);
        }
        nlohmann::json req = {
            {"key", base64(prefix)},
            {"range_end", base64(rangeEnd)},
        };
        auto resp = post(apiPrefix_ + "/kv/range", req.dump());
        std::vector<KV> out;
        if (resp.empty()) return out;
        auto j = nlohmann::json::parse(resp, nullptr, false);
        if (j.is_discarded() || !j.contains("kvs")) return out;
        for (auto& kv : j["kvs"]) {
            KV item;
            if (kv.contains("key"))   item.key   = unbase64(kv["key"].get<std::string>());
            if (kv.contains("value")) item.value = unbase64(kv["value"].get<std::string>());
            out.push_back(std::move(item));
        }
        return out;
    }

    /// 一个简单的"轮询 watch"helper：每 intervalMs 拉一次 prefix，对比上次结果，
    /// 调用 callback(added, removed, updated)。callback 应 quick；底层没分发线程。
    /// 调用方在自己线程里循环调 pollOnce()。
    using ChangeCallback = std::function<void(const std::vector<KV>& added,
                                                const std::vector<std::string>& removedKeys,
                                                const std::vector<KV>& updated)>;

    /// 上一次 pollOnce 的结果缓存（key → value）
    std::unordered_map<std::string, std::string> lastSnapshot_;

    /// 调用一次 prefix 拉取，跟 lastSnapshot_ 对比，回调通知变化
    void pollOnce(const std::string& prefix, const ChangeCallback& cb) {
        auto kvs = getPrefix(prefix);
        std::unordered_map<std::string, std::string> cur;
        for (auto& kv : kvs) cur[kv.key] = kv.value;

        std::vector<KV> added, updated;
        std::vector<std::string> removed;
        for (auto& [k, v] : cur) {
            auto it = lastSnapshot_.find(k);
            if (it == lastSnapshot_.end()) added.push_back({k, v});
            else if (it->second != v)      updated.push_back({k, v});
        }
        for (auto& [k, _] : lastSnapshot_) {
            if (cur.find(k) == cur.end()) removed.push_back(k);
        }
        if (!added.empty() || !removed.empty() || !updated.empty()) {
            cb(added, removed, updated);
        }
        lastSnapshot_ = std::move(cur);
    }

private:
    /// 同步 POST /path JSON body → 返回 response body（空表示失败）
    std::string post(const std::string& path, const std::string& body) {
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return {};
        struct timeval tv{2, 0};  // 2s
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
            ::close(sock);
            return {};
        }
        if (::connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(sock);
            return {};
        }

        // HTTP/1.0 强制不用 chunked transfer encoding（grpc-gateway 默认 chunked）
        std::string req;
        req += "POST " + path + " HTTP/1.0\r\n";
        req += "Host: " + host_ + ":" + std::to_string(port_) + "\r\n";
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "Connection: close\r\n";
        req += "\r\n";
        req += body;
        ssize_t sent = ::send(sock, req.data(), req.size(), 0);
        if (sent < (ssize_t)req.size()) {
            ::close(sock);
            return {};
        }

        // 读完整响应
        std::string resp;
        char buf[4096];
        while (true) {
            ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            resp.append(buf, n);
        }
        ::close(sock);

        // 拆 body
        auto p = resp.find("\r\n\r\n");
        if (p == std::string::npos) return {};
        return resp.substr(p + 4);
    }

    static std::string base64(const std::string& in) {
        static const char* tbl =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        size_t i = 0;
        while (i + 3 <= in.size()) {
            uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i+1] << 8) | (uint8_t)in[i+2];
            out += tbl[(v >> 18) & 0x3F];
            out += tbl[(v >> 12) & 0x3F];
            out += tbl[(v >>  6) & 0x3F];
            out += tbl[ v        & 0x3F];
            i += 3;
        }
        if (i < in.size()) {
            uint32_t v = (uint8_t)in[i] << 16;
            if (i + 1 < in.size()) v |= (uint8_t)in[i+1] << 8;
            out += tbl[(v >> 18) & 0x3F];
            out += tbl[(v >> 12) & 0x3F];
            out += (i + 1 < in.size()) ? tbl[(v >> 6) & 0x3F] : '=';
            out += '=';
        }
        return out;
    }

    static std::string unbase64(const std::string& in) {
        static int dec[256];
        static bool init = false;
        if (!init) {
            for (int i = 0; i < 256; ++i) dec[i] = -1;
            const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            for (int i = 0; i < 64; ++i) dec[(int)tbl[i]] = i;
            init = true;
        }
        std::string out;
        int val = 0, valb = -8;
        for (unsigned char c : in) {
            if (c == '=' || c == '\n' || c == '\r') continue;
            int d = dec[c];
            if (d == -1) continue;
            val = (val << 6) | d;
            valb += 6;
            if (valb >= 0) {
                out += static_cast<char>((val >> valb) & 0xFF);
                valb -= 8;
            }
        }
        return out;
    }

    std::string host_;
    int port_;
    std::string apiPrefix_;
};
