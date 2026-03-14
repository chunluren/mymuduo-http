#include"logger.h"
#include"Timestamp.h"
#include<iostream>
/**
 * @brief 获取Logger类的单例实例
 * 
 * 使用静态局部变量实现线程安全的单例模式，首次调用时创建Logger实例，
 * 后续调用返回已创建的实例引用。
 * 
 * @return Logger& 返回Logger单例对象的引用
 */
Logger& Logger::instance()
{
    // 使用静态局部变量实现线程安全的单例模式
    static Logger logger;
    return logger;
}
//设置日志级别
void Logger::setLogLevel(int level)
{
    logLevel_ = level;
}
//写日志[日志级别]time : msg
void Logger::log(std::string msg)
{
    switch (logLevel_)
    {
        case DEBUG:
            std::cout << "[DEBUG]";
            break;
        case ERROR:
            std::cout << "[ERROR]";
            break;
        case FATAL:
            std::cout << "[FATAL]";
            break;
        case INFO:
            std::cout << "[INFO]";
            break;
    }
    std::cout << Timestamp::now().toString() << msg << std::endl;

}