// Config.h - 配置管理
#pragma once

#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <vector>
#include <mutex>

// 配置值
class ConfigValue {
public:
    ConfigValue() = default;
    explicit ConfigValue(const std::string& value) : value_(value) {}
    
    std::string asString() const { return value_; }
    int asInt() const { return std::stoi(value_); }
    int64_t asInt64() const { return std::stoll(value_); }
    double asDouble() const { return std::stod(value_); }
    bool asBool() const { return value_ == "true" || value_ == "1"; }
    
    std::vector<std::string> asList(char delim = ',') const {
        std::vector<std::string> result;
        std::istringstream iss(value_);
        std::string item;
        while (std::getline(iss, item, delim)) {
            result.push_back(item);
        }
        return result;
    }

private:
    std::string value_;
};

// 配置管理器
class Config {
public:
    static Config& instance() {
        static Config config;
        return config;
    }
    
    // 从文件加载
    bool load(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return false;
        }
        
        std::string line;
        std::string section;
        
        while (std::getline(file, line)) {
            // 去除空白
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t") + 1);
            
            // 跳过空行和注释
            if (line.empty() || line[0] == '#') continue;
            
            // section
            if (line[0] == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2) + ".";
                continue;
            }
            
            // key = value
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string value = line.substr(eq + 1);
                
                // 去除引号
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                    value = value.substr(1, value.size() - 2);
                }
                
                std::lock_guard<std::mutex> lock(mutex_);
                values_[section + key] = ConfigValue(value);
            }
        }
        
        filename_ = filename;
        return true;
    }
    
    // 重新加载
    bool reload() {
        if (filename_.empty()) return false;
        return load(filename_);
    }
    
    // 获取配置
    ConfigValue get(const std::string& key, const std::string& defaultValue = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = values_.find(key);
        return it != values_.end() ? it->second : ConfigValue(defaultValue);
    }
    
    // 设置配置
    void set(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        values_[key] = ConfigValue(value);
    }
    
    // 检查是否存在
    bool has(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return values_.find(key) != values_.end();
    }

private:
    Config() = default;
    
    std::string filename_;
    std::unordered_map<std::string, ConfigValue> values_;
    mutable std::mutex mutex_;
};

// 配置宏
#define CONFIG(key) Config::instance().get(key)
#define CONFIG_INT(key) Config::instance().get(key).asInt()
#define CONFIG_STRING(key) Config::instance().get(key).asString()
#define CONFIG_BOOL(key) Config::instance().get(key).asBool()
#define CONFIG_DOUBLE(key) Config::instance().get(key).asDouble()