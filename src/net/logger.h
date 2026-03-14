#pragma once

#include"noncopyable.h"
#include<string>
//logmsgFormat,arg1,arg2,arg3...
//宏定义换行要加\，后面不能接空格
#define LOG_INFO(logmsgFormat, ...)\
do{ \
    Logger& logger = Logger::instance(); \
    logger.setLogLevel(INFO); \
    char buf[1024] = {0}; \
    snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
    logger.log(buf); \
}while(0)

#define LOG_ERROR(logmsgFormat, ...)\
do{ \
    Logger& logger = Logger::instance(); \
    logger.setLogLevel(ERROR); \
    char buf[1024] = {0}; \
    snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
    logger.log(buf); \
}while(0)

#define LOG_FATAL(logmsgFormat, ...)\
do{ \
    Logger& logger = Logger::instance(); \
    logger.setLogLevel(FATAL); \
    char buf[1024] = {0}; \
    snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
    logger.log(buf); \
    exit(-1);\
}while(0)

/*
int snprintf(char *str, size_t size, const char *format, ...);
成功：返回实际写入的字符数（不含终止符）
错误：返回负值
截断：若内容超出缓冲区大小，会截断并返回完整内容长度
*/

#ifdef MUDEBUG
#define LOG_DEBUG(logmsgFormat, ...)\
do{ \
    Logger& logger = Logger::instance(); \
    logger.setLogLevel(DEBUG); \
    char buf[1024] = {0}; \
    snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
    logger.log(buf); \
}while(0)

#else
    #define LOG_DEBUG(logmsgFormat, ...)do{}while(0)
#endif
//定义日志级别 INFO DEBUG ERROR FATAL
enum LogLvel
{
    INFO, //普通信息
    DEBUG,//调试信息
    ERROR,//错误信息
    FATAL //core信息
};

class Logger : noncopyable
{
public:
    //单例模式
    static Logger& instance();
    //设置日志级别
    void setLogLevel(int level);
    //写日志
    void log(std::string msg);
private:
    int logLevel_;
    Logger(){}
};