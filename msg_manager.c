/**
 * @file msg_manager.c
 * @brief 消息管理器模块实现
 *
 * 实现消息队列的注册、注销和管理功能，
 * 提供线程安全的跨队列消息发送接口。
 */

#include "msg_manager.h"
#include <stdlib.h>
#include <stdio.h>

/** 全局消息管理器实例 */
static msg_manager g_msg_manager = { 0 };

/** 消息池项 */
typedef struct msg_pool_item {
    bool is_used;                /**< 是否正在使用 */
    size_t size;                 /**< 消息大小 */
    uint8_t buffer[64];          /**< 预分配的消息缓冲区 */
} msg_pool_item;

/** 消息池 */
static msg_pool_item msg_pool[MSG_POOL_SIZE] = { 0 };
static StaticSemaphore_t msg_pool_mutex_buffer;
static os_semaphore_handle msg_pool_mutex = NULL;

#ifdef ENABLE_CALLBACK_TIMEOUT
// 任务池配置
#define CALLBACK_TASK_POOL_SIZE 4  // 任务池大小

// 回调任务状态
typedef enum {
    TASK_STATE_IDLE,    // 空闲
    TASK_STATE_BUSY,    // 忙
    TASK_STATE_TIMEOUT  // 超时
} callback_task_state_t;

// 回调任务信息
typedef struct callback_task_info {
    os_task_handle handle;           // 任务句柄
    callback_task_state_t state;   // 任务状态
    msg_base* current_msg;         // 当前处理的消息
    os_timer_handle timeout_timer;   // 超时定时器
    StaticTask_t task_buffer;      // 静态任务缓冲区
    StackType_t task_stack[configMINIMAL_STACK_SIZE * 2];  // 任务栈
} callback_task_info_t;

// 任务池
static callback_task_info_t g_callback_task_pool[CALLBACK_TASK_POOL_SIZE];
static os_semaphore_handle g_task_pool_mutex;  // 任务池互斥锁

/**
 * @brief 回调执行任务
 *
 * 从任务池获取任务信息，执行回调函数
 */
static void prv_callback_task(void *pvParameters);

/**
 * @brief 任务超时回调函数
 */
static void prv_task_timeout_callback(TimerHandle_t xTimer);

/**
 * @brief 初始化任务池
 */
static void prv_init_task_pool(void);

/**
 * @brief 从任务池分配一个空闲任务
 *
 * @return 空闲任务的索引，-1表示无空闲任务
 */
static int prv_allocate_task(void);
#endif /* ENABLE_CALLBACK_TIMEOUT */

/**
 * @brief 查找指定id的队列条目
 *
 * 在管理器中搜索指定id的队列条目
 *
 * @param id 要查找的队列id
 * @return 找到的条目指针，未找到返回NULL
 */
static msg_manager_entry *prv_find_entry_by_id(uint8_t id)
{
    if (id == 0) {
        return NULL;
    }

    os_semaphore_take(g_msg_manager.mutex, OS_WAIT_FOREVER);
    msg_manager_entry *entry = NULL;
    for (int i = 0; i < MSG_MANAGER_MAX_ENTRIES; i++) {
        if (g_msg_manager.entries[i].is_used &&
            g_msg_manager.entries[i].handle.id == id) {
            entry = &g_msg_manager.entries[i];
            break;
        }
    }
    os_semaphore_give(g_msg_manager.mutex);
    return entry;
}


/**
 * @brief 查找空闲的条目
 *
 * 在静态数组中查找一个未使用的条目
 *
 * @return 空闲条目指针，未找到返回NULL
 */
static msg_manager_entry *prv_find_free_entry(void)
{
    os_semaphore_take(g_msg_manager.mutex, OS_WAIT_FOREVER);
    msg_manager_entry *entry = NULL;
    for (int i = 0; i < MSG_MANAGER_MAX_ENTRIES; i++) {
        if (!g_msg_manager.entries[i].is_used) {
            entry = &g_msg_manager.entries[i];
            break;
        }
    }
    os_semaphore_give(g_msg_manager.mutex);
    return entry;
}

/**
 * @brief 消息分发器任务
 *
 * 从全局队列接收消息并根据target_name分发给对应回调函数
 *
 * @param pvParameters 未使用
 */
#ifdef ENABLE_CALLBACK_TIMEOUT
/**
 * @brief 回调执行任务
 *
 * 从任务池获取任务信息，执行回调函数
 */
static void prv_callback_task(void *pvParameters) {
    callback_task_info_t *task_info = (callback_task_info_t *)pvParameters;
    
    for (;;) {
        // 等待任务分配
        os_task_notify_take(true, OS_WAIT_FOREVER);
        
        if (task_info->state == TASK_STATE_BUSY && task_info->current_msg != NULL) {
            // 执行回调函数
            if (task_info->current_msg->callback != NULL) {
                // 使用消息级回调
                task_info->current_msg->callback(task_info->current_msg);
            } else {
                // 使用队列级回调
                msg_manager_entry *entry = prv_find_entry_by_id(task_info->current_msg->target_queue_id);
                if (entry != NULL && entry->callback != NULL) {
                    entry->callback(task_info->current_msg);
                }
            }
            
            // 释放消息
            msg_manager_free_msg(task_info->current_msg);
            
            // 清理状态
            task_info->current_msg = NULL;
            task_info->state = TASK_STATE_IDLE;
        }
    }
}

/**
 * @brief 任务超时回调函数
 */
static void prv_task_timeout_callback(os_timer_handle xTimer) {
    int task_index = (int)os_timer_get_id(xTimer);
    
    if (task_index >= 0 && task_index < CALLBACK_TASK_POOL_SIZE) {
        callback_task_info_t *task_info = &g_callback_task_pool[task_index];
        
        if (task_info->state == TASK_STATE_BUSY) {
            printf("Callback task %d execution timeout!\n", task_index);
            
            // 强制终止任务
            os_task_delete(task_info->handle);
            
            // 释放消息
            if (task_info->current_msg != NULL) {
                msg_manager_free_msg(task_info->current_msg);
                task_info->current_msg = NULL;
            }
            
            // 重新创建任务
            task_info->handle = os_task_create_static(
                "CallbackTask",
                prv_callback_task,
                task_info,
                configMINIMAL_STACK_SIZE * 2,
                MSG_DISPATCHER_PRIORITY + 1,
                task_info->task_stack,
                &task_info->task_buffer
            );
            
            // 重置状态
            task_info->state = TASK_STATE_IDLE;
        }
    }
}

/**
 * @brief 初始化任务池
 */
static void prv_init_task_pool(void) {
    // 创建任务池互斥锁
    g_task_pool_mutex = os_semaphore_create_mutex();
    
    // 初始化任务池
    for (int i = 0; i < CALLBACK_TASK_POOL_SIZE; i++) {
        // 创建回调任务
        g_callback_task_pool[i].handle = os_task_create_static(
            "CallbackTask",
            prv_callback_task,
            &g_callback_task_pool[i],
            configMINIMAL_STACK_SIZE * 2,
            MSG_DISPATCHER_PRIORITY + 1,
            g_callback_task_pool[i].task_stack,
            &g_callback_task_pool[i].task_buffer
        );
        
        // 创建超时定时器
        g_callback_task_pool[i].timeout_timer = os_timer_create(
            "CallbackTimeout",
            os_ms_to_ticks(CALLBACK_TIMEOUT_MS),
            false,
            (void *)i,
            prv_task_timeout_callback
        );
        
        // 初始化为空闲状态
        g_callback_task_pool[i].state = TASK_STATE_IDLE;
        g_callback_task_pool[i].current_msg = NULL;
    }
}

/**
 * @brief 从任务池分配一个空闲任务
 *
 * @return 空闲任务的索引，-1表示无空闲任务
 */
static int prv_allocate_task(void) {
    os_semaphore_take(g_task_pool_mutex, OS_WAIT_FOREVER);
    
    int task_index = -1;
    for (int i = 0; i < CALLBACK_TASK_POOL_SIZE; i++) {
        if (g_callback_task_pool[i].state == TASK_STATE_IDLE) {
            task_index = i;
            g_callback_task_pool[i].state = TASK_STATE_BUSY;
            break;
        }
    }
    
    os_semaphore_give(g_task_pool_mutex);
    return task_index;
}

/**
 * @brief 消息分发器任务
 *
 * 从全局队列接收消息并根据type_id分发给对应回调函数（带超时机制）
 */
static void prv_message_dispatcher(void *pvParameters) {
    (void)pvParameters;
    msg_base *msg;
    
    for (;;) {
        // 从全局队列接收消息
        if (msg_queue_pop(g_msg_manager.global_queue, &msg, POP_BLOCK) == MSG_QUEUE_CODE_OK) {
            // 从任务池分配一个空闲任务
            int task_index = prv_allocate_task();
            if (task_index >= 0) {
                callback_task_info_t *task_info = &g_callback_task_pool[task_index];
                
                // 分配任务
                task_info->current_msg = msg;
                task_info->state = TASK_STATE_BUSY;
                
                // 启动超时定时器
                os_timer_start(task_info->timeout_timer, 0);
                
                // 通知任务开始执行
                os_task_notify_give(task_info->handle);
            } else {
                // 任务池已满，使用默认处理
                msg_manager_entry *entry = prv_find_entry_by_id(msg->target_queue_id);
                if (entry != NULL && entry->callback != NULL) {
                    entry->callback(msg);
                } else {
                    msg_manager_free_msg(msg);
                }
            }
        }
    }
}
#else /* ENABLE_CALLBACK_TIMEOUT */
/**
 * @brief 消息分发器任务
 *
 * 从全局队列接收消息并根据type_id分发给对应回调函数（无超时机制）
 */
static void prv_message_dispatcher(void *pvParameters) {
    (void)pvParameters;

    msg_base *msg;

    for (;;) {
        // 从全局队列接收消息
        if (msg_queue_pop(g_msg_manager.global_queue, &msg, POP_BLOCK) == MSG_QUEUE_CODE_OK) {
            // 优先使用消息级回调
            if (msg->callback != NULL) {
                msg->callback(msg);
            } else {
                // 使用队列级回调
                msg_manager_entry *entry = prv_find_entry_by_id(msg->target_queue_id);
                if (entry != NULL && entry->callback != NULL) {
                    entry->callback(msg);
                }
            }
            // 自动释放消息
            msg_manager_free_msg(msg);
        }
    }
}
#endif /* ENABLE_CALLBACK_TIMEOUT */

/**
 * @brief 初始化消息管理器
 *
 * 创建全局消息管理器实例、互斥锁和全局队列（单队列优化）
 */
void msg_manager_init(void)
{
    if (g_msg_manager.mutex == NULL) {
        // 初始化所有条目为未使用状态
        for (int i = 0; i < MSG_MANAGER_MAX_ENTRIES; i++) {
            g_msg_manager.entries[i].is_used = false;
            g_msg_manager.entries[i].callback = NULL;
            g_msg_manager.entries[i].timeout_ms = -1;
            g_msg_manager.entries[i].handle.id = 0;
        }

        // 创建静态互斥锁
        g_msg_manager.mutex = os_semaphore_create_mutex_static(&g_msg_manager.mutex_buffer);

        // 创建消息池互斥锁
        msg_pool_mutex = os_semaphore_create_mutex_static(&msg_pool_mutex_buffer);

        // 初始化消息池
        for (int i = 0; i < MSG_POOL_SIZE; i++) {
            msg_pool[i].is_used = false;
            msg_pool[i].size = 0;
        }

        // 创建全局消息队列
        g_msg_manager.global_queue = msg_queue_create_static(MSG_QUEUE_MAX_ITEMS);

        // 初始化队列ID计数器
        g_msg_manager.next_queue_id = 1;

#ifdef ENABLE_CALLBACK_TIMEOUT
        // 初始化任务池
        prv_init_task_pool();

        // 创建消息分发器任务
        g_msg_manager.dispatcher_task = os_task_create_static(
            "MsgDispatcher",
            prv_message_dispatcher,
            NULL,
            MSG_DISPATCHER_STACK_SIZE,
            OS_TASK_PRIORITY_NORMAL,
            g_msg_manager.dispatcher_stack,
            &g_msg_manager.dispatcher_task_buffer
        );
#else /* ENABLE_CALLBACK_TIMEOUT */
        // 创建消息分发器任务
        g_msg_manager.dispatcher_task = os_task_create_static(
            "MsgDispatcher",
            prv_message_dispatcher,
            NULL,
            MSG_DISPATCHER_STACK_SIZE,
            OS_TASK_PRIORITY_NORMAL,
            g_msg_manager.dispatcher_stack,
            &g_msg_manager.dispatcher_task_buffer
        );
#endif /* ENABLE_CALLBACK_TIMEOUT */
    }
}

/**
 * @brief 反初始化消息管理器
 *
 * 销毁全局队列、分发器任务并释放资源（单队列优化）
 */
void msg_manager_deinit(void)
{
    if (g_msg_manager.mutex == NULL) {
        return;
    }

    os_semaphore_take(g_msg_manager.mutex, OS_WAIT_FOREVER);

    // 停止分发器任务
    if (g_msg_manager.dispatcher_task != NULL) {
        os_task_delete(g_msg_manager.dispatcher_task);
        g_msg_manager.dispatcher_task = NULL;
    }

    // 销毁全局队列
    if (g_msg_manager.global_queue != NULL) {
        msg_queue_destroy(g_msg_manager.global_queue);
        g_msg_manager.global_queue = NULL;
    }

    // 清除所有条目
    for (int i = 0; i < MSG_MANAGER_MAX_ENTRIES; i++) {
        g_msg_manager.entries[i].is_used = false;
        g_msg_manager.entries[i].callback = NULL;
    }

    os_semaphore_give(g_msg_manager.mutex);

    // 注意：静态互斥锁不需要删除，只需要重置状态
    g_msg_manager.mutex = NULL;
}

/**
 * @brief 检查消息句柄是否有效
 *
 * 验证消息句柄是否有效（非空且id不为0）
 *
 * @param handle 要检查的句柄
 * @return 有效返回true，无效返回false
 */
bool msg_handle_is_valid(const msg_handle *handle)
{
    return (handle != NULL) && (handle->id != 0);
}

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
                                int16_t empty_event_timeout_ms)
{
    if (callback == NULL) {
        return NULL;
    }

    if (g_msg_manager.mutex == NULL) {
        msg_manager_init();
    }

    // 查找空闲条目
    msg_manager_entry *free_entry = prv_find_free_entry();
    if (free_entry == NULL) {
        return NULL;
    }

    // 分配队列ID并注册
    os_semaphore_take(g_msg_manager.mutex, OS_WAIT_FOREVER);
    free_entry->is_used = true;
    free_entry->callback = callback;
    free_entry->timeout_ms = empty_event_timeout_ms;
    free_entry->handle.id = g_msg_manager.next_queue_id++;
    os_semaphore_give(g_msg_manager.mutex);
    return &free_entry->handle;
}

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
                                        int16_t empty_event_timeout_ms)
{
    if (callback == NULL || queue_id == 0) {
        return NULL;
    }

    if (g_msg_manager.mutex == NULL) {
        msg_manager_init();
    }

    // 检查队列ID是否已被使用
    if (prv_find_entry_by_id(queue_id) != NULL) {
        return NULL;
    }

    // 查找空闲条目
    msg_manager_entry *free_entry = prv_find_free_entry();
    if (free_entry == NULL) {
        return NULL;
    }

    // 使用指定的队列ID注册
    os_semaphore_take(g_msg_manager.mutex, OS_WAIT_FOREVER);
    free_entry->is_used = true;
    free_entry->callback = callback;
    free_entry->timeout_ms = empty_event_timeout_ms;
    free_entry->handle.id = queue_id;
    // 更新next_queue_id，确保它大于已使用的最大ID
    if (queue_id >= g_msg_manager.next_queue_id) {
        g_msg_manager.next_queue_id = queue_id + 1;
    }
    os_semaphore_give(g_msg_manager.mutex);
    return &free_entry->handle;
}



/**
 * @brief 通过句柄注销消息队列
 *
 * 从管理器中移除指定句柄的队列（单队列优化：只清除回调映射）
 *
 * @param handle 要注销的队列句柄
 */
void msg_manager_unregister_by_handle(const msg_handle *handle)
{
    if (handle == NULL || g_msg_manager.mutex == NULL) {
        return;
    }

    msg_manager_entry *found_entry = prv_find_entry_by_id(handle->id);
    if (found_entry != NULL) {
        os_semaphore_take(g_msg_manager.mutex, OS_WAIT_FOREVER);
        // 清除回调，标记为未使用
        found_entry->callback = NULL;
        found_entry->is_used = false;
        os_semaphore_give(g_msg_manager.mutex);
    }
}

void msg_manager_unregister_by_id(uint8_t id)
{
    if (g_msg_manager.mutex == NULL) {
        return;
    }

    msg_manager_entry *found_entry = prv_find_entry_by_id(id);
    if (found_entry != NULL) {
        os_semaphore_take(g_msg_manager.mutex, OS_WAIT_FOREVER);
        found_entry->callback = NULL;
        found_entry->is_used = false;
        os_semaphore_give(g_msg_manager.mutex);
    }
}

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
                                msg_base *msg)
{
    if (to == NULL || msg == NULL || g_msg_manager.global_queue == NULL) {
        return MSG_QUEUE_CODE_NOT_EXISTS;
    }

    // 检查目标队列是否存在
    if (prv_find_entry_by_id(to->id) == NULL) {
        return MSG_QUEUE_CODE_NOT_EXISTS;
    }

    // 设置消息的目标ID
    msg->target_queue_id = to->id;

    // 发送到全局队列
    return msg_queue_push(g_msg_manager.global_queue, msg);
}

/**
 * @brief 从消息池获取消息
 * 
 * @param size 消息大小
 * @return 消息指针，失败返回NULL
 */
msg_base* msg_manager_alloc_msg(size_t size)
{
    if (msg_pool_mutex == NULL) {
        msg_manager_init();
    }

    // 检查消息大小是否超过缓冲区大小
    if (size > sizeof(msg_pool[0].buffer)) {
        // 超过预分配大小，使用动态分配
        return (msg_base*)os_malloc(size);
    }

    os_semaphore_take(msg_pool_mutex, OS_WAIT_FOREVER);
    // 查找空闲消息槽
    for (int i = 0; i < MSG_POOL_SIZE; i++) {
        if (!msg_pool[i].is_used) {
            msg_pool[i].is_used = true;
            msg_pool[i].size = size;
            os_semaphore_give(msg_pool_mutex);
            return (msg_base*)msg_pool[i].buffer;
        }
    }
    os_semaphore_give(msg_pool_mutex);
    
    // 消息池已满，使用动态分配
    return (msg_base*)os_malloc(size);
}

/**
 * @brief 释放消息回消息池
 * 
 * @param msg 消息指针
 */
void msg_manager_free_msg(msg_base* msg)
{
    if (msg == NULL || msg_pool_mutex == NULL) {
        return;
    }

    os_semaphore_take(msg_pool_mutex, OS_WAIT_FOREVER);
    // 检查是否是从消息池分配的
    for (int i = 0; i < MSG_POOL_SIZE; i++) {
        if (msg_pool[i].is_used && (msg == (msg_base*)msg_pool[i].buffer)) {
            msg_pool[i].is_used = false;
            msg_pool[i].size = 0;
            os_semaphore_give(msg_pool_mutex);
            return;
        }
    }
    os_semaphore_give(msg_pool_mutex);
    
    // 不是从消息池分配的，使用动态释放
    if (msg->destroy != NULL) {
        msg->destroy(msg);
    } else {
        os_free(msg);
    }
}

/**
 * @brief 发送消息到指定队列（简化版）
 *
 * 向指定接收者发送消息（单队列优化）
 *
 * @param to 接收者队列句柄
 * @param msg 要发送的消息
 * @return 发送结果状态码
 */
msg_queue_code msg_manager_send_msg_to(msg_handle *to, msg_base *msg)
{
    return msg_manager_send_msg(to, msg);
}

/**
 * @brief 通过队列ID发送消息
 *
 * 向指定队列ID的接收者发送消息
 *
 * @param queue_id 接收者队列ID
 * @param msg 要发送的消息
 * @return 发送结果状态码
 */
msg_queue_code msg_manager_send_msg_to_id(uint8_t queue_id, msg_base *msg)
{
    if (msg == NULL || g_msg_manager.global_queue == NULL) {
        return MSG_QUEUE_CODE_NOT_EXISTS;
    }

    // 检查目标队列是否存在
    if (prv_find_entry_by_id(queue_id) == NULL) {
        return MSG_QUEUE_CODE_NOT_EXISTS;
    }

    // 设置消息的目标ID
    msg->target_queue_id = queue_id;

    // 发送到全局队列
    return msg_queue_push(g_msg_manager.global_queue, msg);
}

/**
 * @brief 获取队列大小
 *
 * 查询全局队列中当前的消息数量（单队列优化）
 *
 * @param name 队列名称（保留参数，单队列中不区分）
 * @return 全局队列中的消息数量
 */
int msg_manager_size(void)
{
    if (g_msg_manager.global_queue == NULL) {
        return 0;
    }

    return msg_queue_size(g_msg_manager.global_queue);
}

/**
 * @brief 清除全局队列中的所有消息
 *
 * 清除全局队列中的所有消息，并释放消息内存
 */
void msg_manager_clear_all_messages(void)
{
    if (g_msg_manager.global_queue == NULL) {
        return;
    }

    msg_base *msg;
    while (msg_queue_pop(g_msg_manager.global_queue, &msg, POP_NONE_BLOCK) == MSG_QUEUE_CODE_OK) {
        msg_manager_free_msg(msg);
    }
}

/**
 * @brief 清除指定类型的消息
 *
 * @param type_id 消息类型ID
 */
void msg_manager_clear_messages_by_type(uint8_t type_id)
{
    if (g_msg_manager.global_queue == NULL) {
        return;
    }

    // 临时队列用于存储非目标类型的消息
    msg_queue_handle temp_queue = msg_queue_create(MSG_QUEUE_MAX_ITEMS);
    if (temp_queue == NULL) {
        return;
    }

    // 分离消息
    msg_base *msg;
    while (msg_queue_pop(g_msg_manager.global_queue, &msg, POP_NONE_BLOCK) == MSG_QUEUE_CODE_OK) {
        if (msg->type_id == type_id) {
            msg_manager_free_msg(msg);
        } else {
            msg_queue_push(temp_queue, msg);
        }
    }

    // 将非目标类型消息放回原队列
    while (msg_queue_pop(temp_queue, &msg, POP_NONE_BLOCK) == MSG_QUEUE_CODE_OK) {
        msg_queue_push(g_msg_manager.global_queue, msg);
    }

    msg_queue_destroy(temp_queue);
}

/**
 * @brief 清除发送到指定队列的消息
 *
 * @param handle 目标队列句柄
 */
void msg_manager_clear_messages_by_queue(msg_handle *handle)
{
    if (g_msg_manager.global_queue == NULL || handle == NULL) {
        return;
    }

    // 临时队列用于存储非目标队列的消息
    msg_queue_handle temp_queue = msg_queue_create(MSG_QUEUE_MAX_ITEMS);
    if (temp_queue == NULL) {
        return;
    }

    // 分离消息
    msg_base *msg;
    while (msg_queue_pop(g_msg_manager.global_queue, &msg, POP_NONE_BLOCK) == MSG_QUEUE_CODE_OK) {
        if (msg->target_queue_id == handle->id) {
            msg_manager_free_msg(msg);
        } else {
            msg_queue_push(temp_queue, msg);
        }
    }

    // 将非目标队列消息放回原队列
    while (msg_queue_pop(temp_queue, &msg, POP_NONE_BLOCK) == MSG_QUEUE_CODE_OK) {
        msg_queue_push(g_msg_manager.global_queue, msg);
    }

    msg_queue_destroy(temp_queue);
}