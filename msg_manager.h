/**
 * @file msg_manager.h
 * @brief 消息管理器模块头文件
 *
 * 该模块提供消息队列的管理功能，支持注册、注销消息队列，
 * 并提供跨队列的消息发送接口。类似于C++中的单例管理器。
 */

#ifndef MSG_MANAGER_H
#define MSG_MANAGER_H

#include "os_adapter.h"
#include "msg_queue.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/** 消息管理器支持的最大队列数量 */
#define MSG_MANAGER_MAX_ENTRIES 2

/** 消息分发器任务栈大小 */
#define MSG_DISPATCHER_STACK_SIZE 128

/** 消息池大小 */
#define MSG_POOL_SIZE 10

/** 消息分发器任务优先级 */
#define MSG_DISPATCHER_PRIORITY (OS_TASK_PRIORITY_IDLE + 1)



/** 回调超时机制配置 */
/* 定义此宏启用回调超时机制，防止回调函数阻塞消息处理 */
//#define ENABLE_CALLBACK_TIMEOUT

/** 回调超时时间（毫秒） */
#define CALLBACK_TIMEOUT_MS 1000

/** 定义此宏启用真正中断阻塞回调的功能 */
//#define ENABLE_CALLBACK_INTERRUPT

/**
 * @brief 消息句柄结构体
 *
 * 用于标识和管理已注册的消息队列
 */
typedef struct msg_handle
{
    uint8_t id;                        /**< 队列唯一标识 */
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
    os_semaphore_handle mutex;                             /**< 保护并发访问的互斥锁 */
    msg_queue_handle global_queue;                       /**< 全局消息队列 */
    os_task_handle dispatcher_task;                        /**< 消息分发器任务 */
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
 * @param callback 消息处理回调函数
 * @param empty_event_timeout_ms 队列空事件超时时间(毫秒)，-1表示无超时
 * @return 注册成功返回句柄，失败返回NULL
 */
msg_handle* msg_manager_register(msg_callback callback,
                                int16_t empty_event_timeout_ms);

/**
 * @brief 使用指定ID注册消息队列
 *
 * 使用指定的队列ID注册消息队列
 *
 * @param queue_id 队列ID（使用msg_queue_id_t枚举值）
 * @param callback 消息处理回调函数
 * @param empty_event_timeout_ms 队列空事件超时时间(毫秒)，-1表示无超时
 * @return 注册成功返回句柄，失败返回NULL
 */
msg_handle* msg_manager_register_with_id(uint8_t queue_id, msg_callback callback,
                                        int16_t empty_event_timeout_ms);

/**
 * @brief 通过名称注销消息队列（保留接口，实际使用id）
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
 * @param to 接收者队列句柄
 * @param msg 要发送的消息
 * @return 发送结果状态码
 */
msg_queue_code msg_manager_send_msg(msg_handle *to,
                                msg_base *msg);

/**
 * @brief 发送消息到指定队列（简化版）
 *
 * 向指定接收者发送消息
 *
 * @param to 接收者队列句柄
 * @param msg 要发送的消息
 * @return 发送结果状态码
 */
msg_queue_code msg_manager_send_msg_to(msg_handle *to, msg_base *msg);

/**
 * @brief 通过队列ID发送消息
 *
 * 向指定队列ID的接收者发送消息
 *
 * @param queue_id 接收者队列ID
 * @param msg 要发送的消息
 * @return 发送结果状态码
 */
msg_queue_code msg_manager_send_msg_to_id(uint8_t queue_id, msg_base *msg);

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
 * 验证消息句柄是否有效（非空且id不为0）
 *
 * @param handle 要检查的句柄
 * @return 有效返回true，无效返回false
 */
bool msg_handle_is_valid(const msg_handle *handle);

/**
 * @brief 清除全局队列中的所有消息
 *
 * 清除全局队列中的所有消息，并释放消息内存
 */
void msg_manager_clear_all_messages(void);

/**
 * @brief 清除指定类型的消息
 *
 * @param type_id 消息类型ID
 */
void msg_manager_clear_messages_by_type(uint8_t type_id);

/**
 * @brief 清除发送到指定队列的消息
 *
 * @param handle 目标队列句柄
 */
void msg_manager_clear_messages_by_queue(msg_handle *handle);

#endif /* MSG_MANAGER_H */