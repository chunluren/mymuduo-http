#pragma once

/**
 * @brief 不可复制类基类
 * 
 * 该类用于作为基类，使得派生类对象无法被复制构造和赋值操作。
 * 通过将拷贝构造函数和拷贝赋值运算符声明为delete，禁止了对象的复制行为。
 */
class noncopyable
{
public:
    noncopyable(const noncopyable&) = delete;
    noncopyable& operator=(const noncopyable& ) = delete;
    
protected:    
    /**
     * @brief 默认构造函数
     * 声明为protected使得该类只能被继承使用，不能直接实例化
     */
    noncopyable() = default;
    ~noncopyable() = default;
};