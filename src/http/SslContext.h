/**
 * @file SslContext.h
 * @brief SSL/TLS 上下文管理（RAII）
 *
 * 封装 OpenSSL 的 SSL_CTX，负责:
 * - 初始化 OpenSSL 库
 * - 创建和管理 SSL_CTX 对象
 * - 加载证书和私钥
 * - 安全默认设置（最低 TLS 1.2）
 *
 * @example 使用示例
 * @code
 * SslContext ctx;
 * if (!ctx.loadCert("/path/to/cert.pem", "/path/to/key.pem")) {
 *     // 处理错误
 * }
 * SSL* ssl = SSL_new(ctx.get());
 * @endcode
 */

#pragma once

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <string>
#include <stdexcept>

/**
 * @class SslContext
 * @brief SSL_CTX RAII 包装器
 *
 * 管理 OpenSSL SSL_CTX 生命周期，提供安全默认配置。
 * 不可拷贝，确保唯一所有权。
 */
class SslContext {
public:
    /**
     * @brief 构造函数，初始化 OpenSSL 并创建 SSL_CTX
     * @throws std::runtime_error 如果创建 SSL_CTX 失败
     *
     * 默认配置:
     * - 最低协议版本: TLS 1.2
     * - 禁用 SSLv2/SSLv3
     */
    SslContext() {
        /// OpenSSL 1.1.0+ 已自动初始化，显式调用是为了兼容旧版本
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();

        /// 使用 TLS_server_method() 创建服务端 SSL 上下文，
        /// 该方法支持 TLS 1.0 ~ TLS 1.3 的所有版本协商
        ctx_ = SSL_CTX_new(TLS_server_method());
        if (!ctx_) {
            throw std::runtime_error("Failed to create SSL_CTX: " + getOpenSslError());
        }

        /// 安全默认设置:
        /// - 最低协议版本设为 TLS 1.2，拒绝不安全的旧协议
        /// - SSL_OP_NO_SSLv2/SSLv3: 双重保险禁用 SSLv2/v3
        /// - SSL_OP_NO_COMPRESSION: 禁用 TLS 压缩（防止 CRIME 攻击）
        /// - SSL_OP_CIPHER_SERVER_PREFERENCE: 优先使用服务端的密码套件顺序
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_options(ctx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                                  SSL_OP_NO_COMPRESSION |
                                  SSL_OP_CIPHER_SERVER_PREFERENCE);
    }

    /**
     * @brief 析构函数，释放 SSL_CTX
     */
    ~SslContext() {
        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }

    // 不可拷贝
    SslContext(const SslContext&) = delete;
    SslContext& operator=(const SslContext&) = delete;

    // 可移动
    SslContext(SslContext&& other) noexcept : ctx_(other.ctx_) {
        other.ctx_ = nullptr;
    }
    SslContext& operator=(SslContext&& other) noexcept {
        if (this != &other) {
            if (ctx_) SSL_CTX_free(ctx_);
            ctx_ = other.ctx_;
            other.ctx_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief 加载证书和私钥文件
     * @param certFile PEM 格式证书文件路径
     * @param keyFile PEM 格式私钥文件路径
     * @return true 加载成功，false 加载失败
     *
     * 加载顺序: 证书 -> 私钥 -> 验证匹配
     */
    bool loadCert(const std::string& certFile, const std::string& keyFile) {
        /// 第一步: 加载 PEM 格式的服务端证书（包含公钥）
        if (SSL_CTX_use_certificate_file(ctx_, certFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
            return false;
        }
        /// 第二步: 加载 PEM 格式的私钥文件
        if (SSL_CTX_use_PrivateKey_file(ctx_, keyFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
            return false;
        }
        /// 第三步: 验证私钥与证书是否匹配（公钥/私钥对一致性检查）
        if (SSL_CTX_check_private_key(ctx_) != 1) {
            return false;
        }
        return true;
    }

    /**
     * @brief 加载证书链文件（包含中间证书）
     * @param chainFile PEM 格式证书链文件路径
     * @return true 加载成功，false 加载失败
     */
    bool loadCertChain(const std::string& chainFile) {
        return SSL_CTX_use_certificate_chain_file(ctx_, chainFile.c_str()) == 1;
    }

    /**
     * @brief 设置密码套件（用于 TLS 1.2 及以下）
     * @param ciphers OpenSSL 格式的密码套件字符串
     * @return true 设置成功
     */
    bool setCiphers(const std::string& ciphers) {
        return SSL_CTX_set_cipher_list(ctx_, ciphers.c_str()) == 1;
    }

    /**
     * @brief 设置 TLS 1.3 密码套件
     * @param ciphersuites TLS 1.3 密码套件字符串
     * @return true 设置成功
     */
    bool setCiphersuites(const std::string& ciphersuites) {
        return SSL_CTX_set_ciphersuites(ctx_, ciphersuites.c_str()) == 1;
    }

    /**
     * @brief 获取底层 SSL_CTX 指针
     * @return SSL_CTX* 指针
     */
    SSL_CTX* get() const { return ctx_; }

private:
    SSL_CTX* ctx_ = nullptr;

    /**
     * @brief 获取 OpenSSL 错误信息
     * @return 错误描述字符串
     */
    static std::string getOpenSslError() {
        unsigned long err = ERR_get_error();
        if (err == 0) return "unknown error";
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        return std::string(buf);
    }
};
