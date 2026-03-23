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

// 消息类型枚举
typedef enum {
    MSG_TYPE_DATA = 1,
    MSG_TYPE_TIMEOUT = 2
} msg_type;

// 对应 MsgBase
typedef struct msg_base {
    // 虚析构函数模拟
    void (*destroy)(struct msg_base* self);
    // 添加类型 ID 用于运行时识别
    int type_id;
} msg_base;

// 对应 TimeoutMsg
typedef struct timeout_msg {
    msg_base base;
    int timeout_ms;
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
typedef struct msg_queue_obj msg_queue_obj;
typedef msg_queue_obj* msg_queue_handle;

/**
 * @brief 创建消息队列
 * @param max_size 最大消息数量 (对应 SetMaxSize)
 * @return 句柄，失败返回 NULL
 */
msg_queue_handle msg_queue_create(uint32_t max_size);

/**
 * @brief 销毁消息队列，停止线程，清理剩余消息
 */
void msg_queue_destroy(msg_queue_handle h_queue);

/**
 * @brief 启动后台处理线程 (对应 BenewThread 启动)
 */
void msg_queue_start(msg_queue_handle h_queue);

/**
 * @brief 停止后台线程 (不销毁对象，仅停止循环)
 */
void msg_queue_stop(msg_queue_handle h_queue);

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
msg_queue_code msg_queue_push_with_timeout(msg_queue_handle h_queue, msg_base* msg, int timeout_ms);

/**
 * @brief 弹出消息 (供外部主动拉取)
 * 对应 Pop
 */
msg_queue_code msg_queue_pop(msg_queue_handle h_queue, msg_base** out_msg, pop_type pop_type);

/**
 * @brief 设置消息处理回调
 */
void msg_queue_set_callback(msg_queue_handle h_queue, msg_callback cb);

/**
 * @brief 设置获取消息的超时时间 (用于生成 TimeoutMsg)
 * 对应 SetGetMsgTimeoutMs
 */
void msg_queue_set_get_msg_timeout_ms(msg_queue_handle h_queue, int ms);

/**
 * @brief 设置推送消息的默认超时时间
 * 对应 SetPushTimeoutMs
 */
void msg_queue_set_push_timeout_ms(msg_queue_handle h_queue, int ms);

/**
 * @brief 获取当前队列消息数量
 */
int msg_queue_size(msg_queue_handle h_queue);

/**
 * @brief 辅助函数：创建超时消息
 * 用户也可以自己 malloc 并初始化，此函数方便使用
 */
timeout_msg* timeout_msg_create(int ms);

#ifdef __cplusplus
}
#endif

#endif /* MSG_QUEUE_H */
