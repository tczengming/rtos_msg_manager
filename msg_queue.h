#ifndef MSG_QUEUE_H
#define MSG_QUEUE_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 常量定义
#define MSG_QUEUE_NAME_MAX_LEN 8
#define MSG_QUEUE_STORAGE_SIZE 20
#define MSG_QUEUE_ITEM_SIZE sizeof(msg_base*)
#define MSG_QUEUE_MAX_ITEMS MSG_QUEUE_STORAGE_SIZE
#define MSG_QUEUE_BUFFER_SIZE (MSG_QUEUE_MAX_ITEMS * MSG_QUEUE_ITEM_SIZE)

// 消息类型枚举
typedef enum {
    MSG_TYPE_DATA = 1,
    MSG_TYPE_TIMEOUT = 2
} msg_type;

// 对应 MsgBase
typedef struct msg_base {
    // 虚析构函数模拟
    void (*destroy)(struct msg_base* self);
    // 添加类型 ID 用于运行时识别和目标队列ID
    uint8_t type_id;
} msg_base;

// 对应 TimeoutMsg
typedef struct timeout_msg {
    msg_base base;
    uint16_t timeout_ms;
} timeout_msg;

// 对应 MsgQueue::MsgQueueCode
typedef enum {
    MSG_QUEUE_CODE_OK = 0,
    MSG_QUEUE_CODE_REACH_MAX_SIZE = 1,
    MSG_QUEUE_CODE_NOT_EXISTS = 2,
    MSG_QUEUE_CODE_LOCK_FAILED = 3, // 在 RTOS 中通常映射为超时或失败
    MSG_QUEUE_CODE_EMPTY = 4,
    MSG_QUEUE_CODE_ERROR = 5
} msg_queue_code;

typedef enum {
    POP_NONE_BLOCK,
    POP_BLOCK
} pop_type;

// 回调函数类型: void (*callFunc)(MsgBase* msg)
// 约定：回调结束后，如果消息是动态分配的，调用者或队列逻辑需负责调用 Destroy。
// 在本实现中，队列任务会在回调后自动调用 Destroy 以模拟 unique_ptr 生命周期结束。
typedef void (*msg_callback)(msg_base* msg);

// 消息队列对象 (Opaque Handle)
typedef struct msg_queue_obj {
    QueueHandle_t x_queue;                    /**< FreeRTOS 队列，存储 msg_base* */

    volatile bool b_stop;                      /**< 停止标志 */
    bool b_static;                             /**< 是否为静态创建 */

    // 配置参数
    uint8_t ul_max_size;                      /**< 最大消息数量 */
    int16_t i_get_msg_timeout_ms;             /**< 接收超时 (生成 timeout_msg) */
    int16_t i_push_timeout_ms;                /**< 发送默认超时 */

    // 回调
    msg_callback pfn_callback;                 /**< 消息处理回调函数 */

    // 静态缓冲区
    StaticQueue_t x_queue_buffer;             /**< 静态队列缓冲区 */
    uint8_t queue_storage[sizeof(msg_base*) * MSG_QUEUE_STORAGE_SIZE]; /**< 队列存储空间 */
    StaticTask_t x_task_buffer;               /**< 静态任务缓冲区 */
    StackType_t task_stack[configMINIMAL_STACK_SIZE * 4]; /**< 任务栈空间 */
} msg_queue_obj;

typedef msg_queue_obj* msg_queue_handle;

/**
 * @brief 创建消息队列
 * @param max_size 最大消息数量 (对应 SetMaxSize)
 * @return 句柄，失败返回 NULL
 */
msg_queue_handle msg_queue_create(uint32_t max_size);

/**
 * @brief 创建静态消息队列（单队列优化）
 * @param max_size 最大消息数量
 * @return 句柄，失败返回 NULL
 */
msg_queue_handle msg_queue_create_static(uint32_t max_size);

/**
 * @brief 销毁消息队列，停止线程，清理剩余消息
 */
void msg_queue_destroy(msg_queue_handle h_queue);



/**
 * @brief 推送消息 (非阻塞尝试)
 * 对应 TryPush
 */
msg_queue_code msg_queue_try_push(msg_queue_handle h_queue, msg_base* msg);

/**
 * @brief 推送消息 (根据配置的超时策略)
 * 对应 Push (内部判断 m_pushTimeoutMs)
 */
msg_queue_code msg_queue_push(msg_queue_handle h_queue, msg_base* msg);

/**
 * @brief 阻塞推送
 * 对应 PushBlock
 */
msg_queue_code msg_queue_push_block(msg_queue_handle h_queue, msg_base* msg);

/**
 * @brief 带超时推送
 * 对应 PushWithTimeout
 */
msg_queue_code msg_queue_push_with_timeout(msg_queue_handle h_queue, msg_base* msg, int16_t timeout_ms);

/**
 * @brief 弹出消息 (供外部主动拉取)
 * 对应 Pop
 */
msg_queue_code msg_queue_pop(msg_queue_handle h_queue, msg_base** out_msg, pop_type pop_type);

/**
 * @brief 设置消息处理回调
 * @param h_queue 队列句柄
 * @param cb 回调函数
 */
static inline void msg_queue_set_callback(msg_queue_handle h_queue, msg_callback cb) {
    if (h_queue) h_queue->pfn_callback = cb;
}

/**
 * @brief 设置获取消息的超时时间 (用于生成 TimeoutMsg)
 * 对应 SetGetMsgTimeoutMs
 * @param h_queue 队列句柄
 * @param ms 超时时间(毫秒)，-1表示无超时
 */
static inline void msg_queue_set_get_msg_timeout_ms(msg_queue_handle h_queue, int16_t ms) {
    if (h_queue) h_queue->i_get_msg_timeout_ms = ms;
}

/**
 * @brief 设置推送消息的默认超时时间
 * 对应 SetPushTimeoutMs
 * @param h_queue 队列句柄
 * @param ms 超时时间(毫秒)
 */
static inline void msg_queue_set_push_timeout_ms(msg_queue_handle h_queue, int16_t ms) {
    if (h_queue) h_queue->i_push_timeout_ms = ms;
}

/**
 * @brief 获取当前队列消息数量
 * @param h_queue 队列句柄
 * @return 队列中的消息数量
 */
static inline int msg_queue_size(msg_queue_handle h_queue) {
    if (h_queue == NULL) return 0;
    return (int)uxQueueMessagesWaiting(h_queue->x_queue);
}

/**
 * @brief 辅助函数：创建超时消息
 * 用户也可以自己 malloc 并初始化，此函数方便使用
 */
timeout_msg* timeout_msg_create(int16_t ms);

#ifdef __cplusplus
}
#endif

#endif /* MSG_QUEUE_H */