/**
 * @file msg_manager.h
 * @brief 消息管理器模块头文件
 *
 * 该模块提供消息队列的管理功能，支持注册、注销消息队列，
 * 并提供跨队列的消息发送接口。类似于C++中的单例管理器。
 */

#ifndef MSG_MANAGER_H
#define MSG_MANAGER_H

#include "FreeRTOS.h"
#include "semphr.h"
#include "msg_queue.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/** 消息队列名称最大长度 */
#define MSG_MANAGER_NAME_MAX_LEN MSG_QUEUE_NAME_MAX_LEN

/** 消息管理器支持的最大队列数量 */
#define MSG_MANAGER_MAX_ENTRIES 2

/** 消息分发器任务栈大小 */
#define MSG_DISPATCHER_STACK_SIZE 128

/** 消息池大小 */
#define MSG_POOL_SIZE 10

/** 消息分发器任务优先级 */
#define MSG_DISPATCHER_PRIORITY (tskIDLE_PRIORITY + 1)

/**
 * @brief 消息句柄结构体
 *
 * 用于标识和管理已注册的消息队列
 */
typedef struct msg_handle
{
    uint8_t id;                        /**< 队列唯一标识 */
    char name[MSG_MANAGER_NAME_MAX_LEN]; /**< 队列名称 */
} msg_handle;

/**
 * @brief 消息管理器条目结构体
 *
 * 表示一个已注册的消息队列条目（单队列优化：只保存回调映射）
 */
typedef struct msg_manager_entry {
    msg_callback callback;       /**< 消息处理回调函数 */
    msg_handle handle;           /**< 消息句柄 */
    int16_t timeout_ms;          /**< 空队列超时时间 */
    bool is_used;                /**< 是否正在使用 */
} msg_manager_entry;

/**
 * @brief 消息管理器结构体
 *
 * 全局消息管理器，维护所有已注册的消息队列（单队列优化）
 */
typedef struct msg_manager
{
    msg_manager_entry entries[MSG_MANAGER_MAX_ENTRIES]; /**< 静态条目数组 */
    StaticSemaphore_t mutex_buffer;                      /**< 静态互斥锁缓冲区 */
    SemaphoreHandle_t mutex;                             /**< 保护并发访问的互斥锁 */
    msg_queue_handle global_queue;                       /**< 全局消息队列 */
    TaskHandle_t dispatcher_task;                        /**< 消息分发器任务 */
    StaticTask_t dispatcher_task_buffer;                 /**< 分发器任务静态缓冲区 */
    StackType_t dispatcher_stack[MSG_DISPATCHER_STACK_SIZE]; /**< 分发器任务栈 */
    uint8_t next_queue_id;                               /**< 下一个队列ID */
} msg_manager;

/**
 * @brief 初始化消息管理器
 *
 * 创建全局消息管理器实例和互斥锁
 */
void msg_manager_init(void);

/**
 * @brief 反初始化消息管理器
 *
 * 销毁所有已注册的队列并释放资源
 */
void msg_manager_deinit(void);

/**
 * @brief 注册消息队列
 *
 * 将消息队列注册到管理器中（单队列优化：只保存回调映射）
 *
 * @param name 队列名称，用于后续查找
 * @param callback 消息处理回调函数
 * @param empty_event_timeout_ms 队列空事件超时时间(毫秒)，-1表示无超时
 * @return 注册成功返回true，失败返回false
 */
bool msg_manager_register(const char *name,
                         msg_callback callback,
                         int16_t empty_event_timeout_ms);

/**
 * @brief 通过名称注销消息队列
 *
 * 从管理器中移除指定名称的队列并销毁它
 *
 * @param name 要注销的队列名称
 */
void msg_manager_unregister_by_name(const char *name);

/**
 * @brief 通过句柄注销消息队列
 *
 * 从管理器中移除指定句柄的队列并销毁它
 *
 * @param handle 要注销的队列句柄
 */
void msg_manager_unregister_by_handle(const msg_handle *handle);

/**
 * @brief 通过ID注销消息队列
 *
 * 从管理器中移除指定ID的队列并销毁它
 *
 * @param id 要注销的队列ID
 */
void msg_manager_unregister_by_id(uint8_t id);

/**
 * @brief 从消息池获取消息
 * 
 * @param size 消息大小
 * @return 消息指针，失败返回NULL
 */
msg_base* msg_manager_alloc_msg(size_t size);

/**
 * @brief 释放消息回消息池
 * 
 * @param msg 消息指针
 */
void msg_manager_free_msg(msg_base* msg);

/**
 * @brief 发送消息到指定队列
 *
 * 向指定接收者发送消息（单队列优化）
 *
 * @param to 接收者队列名称
 * @param msg 要发送的消息
 * @return 发送结果状态码
 */
msg_queue_code msg_manager_send_msg(const char *to,
                                msg_base *msg);

/**
 * @brief 发送消息到指定队列（简化版）
 *
 * 向指定接收者发送消息，不指定发送者
 *
 * @param to 接收者队列名称
 * @param msg 要发送的消息
 * @return 发送结果状态码
 */
msg_queue_code msg_manager_send_msg_to(const char *to, msg_base *msg);

/**
 * @brief 获取队列大小
 *
 * 查询全局队列中当前的消息数量（单队列优化）
 *
 * @return 全局队列中的消息数量
 */
int msg_manager_size(void);

/**
 * @brief 检查消息句柄是否有效
 *
 * 验证消息句柄是否有效（非空且名称不为空）
 *
 * @param handle 要检查的句柄
 * @return 有效返回true，无效返回false
 */
bool msg_handle_is_valid(const msg_handle *handle);

#endif /* MSG_MANAGER_H */