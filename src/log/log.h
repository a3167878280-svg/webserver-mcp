#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

class Log
{
public:
    //C++11以后,使用局部变量懒汉不用加锁
    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }

    static void *flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }

    static void *persist_thread(void *args)
    {
        Log::get_instance()->persist_write_loop();
    }

    //可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    void write_log(int level, const char *format, ...);

    void flush(void);

    /**
     * 异步持久化 — 将任意数据写入指定文件（走独立持久化队列 + 独立线程）
     *
     * 和 write_log 的区别:
     *   write_log — 格式化日志行 → 追加到统一的日志文件
     *   persist   — 原始数据 → 覆盖写入到任意指定文件
     *
     * 用于对话管理器的异步落盘: 持久化队列满时降级为同步写
     *
     * @param file_path  目标文件路径 (如 "logs/conv_abc123.json")
     * @param content    要写入的完整内容 (覆盖模式)
     */
    void persist(const std::string& file_path, const std::string& content);

private:
    Log();
    virtual ~Log();
    void *async_write_log()
    {
        string single_log;
        //从阻塞队列中取出一个日志string，写入文件
        while (m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }

    /**
     * 持久化写循环 — 独立线程，从 persist 队列取任务并落盘
     *
     * 队列条目格式: "file_path\ncontent"
     * 第一个 '\n' 之前是目标文件路径，之后是写入内容
     */
    void *persist_write_loop();

private:
    char dir_name[128]; //路径名
    char log_name[128]; //log文件名
    int m_split_lines;  //日志最大行数
    int m_log_buf_size; //日志缓冲区大小
    long long m_count;  //日志行数记录
    int m_today;        //因为按天分类,记录当前时间是那一天
    FILE *m_fp;         //打开log的文件指针
    char *m_buf;
    block_queue<string> *m_log_queue;     //日志阻塞队列
    block_queue<string> *m_persist_queue; //持久化阻塞队列
    bool m_is_async;                      //是否同步标志位
    locker m_mutex;
    int m_close_log; //关闭日志
};

#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}

#endif
