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

/** 消息池, 减少动态内存分配 */
static msg_pool_item msg_pool[MSG_POOL_SIZE] = { 0 };
static StaticSemaphore_t msg_pool_mutex_buffer;
static SemaphoreHandle_t msg_pool_mutex = NULL;

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

    xSemaphoreTake(g_msg_manager.mutex, portMAX_DELAY);
    msg_manager_entry *entry = NULL;
    for (int i = 0; i < MSG_MANAGER_MAX_ENTRIES; i++) {
        if (g_msg_manager.entries[i].is_used &&
            g_msg_manager.entries[i].handle.id == id) {
            entry = &g_msg_manager.entries[i];
            break;
        }
    }
    xSemaphoreGive(g_msg_manager.mutex);
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
    xSemaphoreTake(g_msg_manager.mutex, portMAX_DELAY);
    msg_manager_entry *entry = NULL;
    for (int i = 0; i < MSG_MANAGER_MAX_ENTRIES; i++) {
        if (!g_msg_manager.entries[i].is_used) {
            entry = &g_msg_manager.entries[i];
            break;
        }
    }
    xSemaphoreGive(g_msg_manager.mutex);
    return entry;
}

/**
 * @brief 消息分发器任务
 *
 * 从全局队列接收消息并根据target_name分发给对应回调函数
 *
 * @param pvParameters 未使用
 */
static void prv_message_dispatcher(void *pvParameters)
{
    (void)pvParameters;

    msg_base *msg;

    for (;;) {
        // 从全局队列接收消息
        if (msg_queue_pop(g_msg_manager.global_queue, &msg, POP_BLOCK) == MSG_QUEUE_CODE_OK) {
            // 根据type_id查找对应的回调函数（type_id存储目标队列ID）
            msg_manager_entry *entry = prv_find_entry_by_id(msg->type_id);
            if (entry != NULL && entry->callback != NULL) {
                // 调用回调函数处理消息
                entry->callback(msg);
            } else {
                // 未找到对应条目，销毁消息
                if (msg->destroy != NULL) {
                    msg->destroy(msg);
                } else {
                    msg_manager_free_msg(msg);
                }
            }
        }
    }
}

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
        g_msg_manager.mutex = xSemaphoreCreateMutexStatic(&g_msg_manager.mutex_buffer);

        // 创建消息池互斥锁
        msg_pool_mutex = xSemaphoreCreateMutexStatic(&msg_pool_mutex_buffer);

        // 初始化消息池
        for (int i = 0; i < MSG_POOL_SIZE; i++) {
            msg_pool[i].is_used = false;
            msg_pool[i].size = 0;
        }

        // 创建全局消息队列
        g_msg_manager.global_queue = msg_queue_create_static(MSG_QUEUE_MAX_ITEMS);

        // 初始化队列ID计数器
        g_msg_manager.next_queue_id = 1;

        // 创建消息分发器任务
        g_msg_manager.dispatcher_task = xTaskCreateStatic(
            prv_message_dispatcher,
            "MsgDispatcher",
            MSG_DISPATCHER_STACK_SIZE,
            NULL,
            MSG_DISPATCHER_PRIORITY,
            g_msg_manager.dispatcher_stack,
            &g_msg_manager.dispatcher_task_buffer
        );
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

    xSemaphoreTake(g_msg_manager.mutex, portMAX_DELAY);

    // 停止分发器任务
    if (g_msg_manager.dispatcher_task != NULL) {
        vTaskDelete(g_msg_manager.dispatcher_task);
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

    xSemaphoreGive(g_msg_manager.mutex);

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
    xSemaphoreTake(g_msg_manager.mutex, portMAX_DELAY);
    free_entry->is_used = true;
    free_entry->callback = callback;
    free_entry->timeout_ms = empty_event_timeout_ms;
    free_entry->handle.id = g_msg_manager.next_queue_id++;
    xSemaphoreGive(g_msg_manager.mutex);
    return &free_entry->handle;
}

/**
 * @brief 通过名称注销消息队列（保留接口）
 *
 * 从管理器中移除指定名称的队列（单队列优化：只清除回调映射）
 *
 * @param name 要注销的队列名称
 */
void msg_manager_unregister_by_name(const char *name)
{
    // 由于不再使用name，此函数不做任何操作
    (void)name;
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
        xSemaphoreTake(g_msg_manager.mutex, portMAX_DELAY);
        // 清除回调，标记为未使用
        found_entry->callback = NULL;
        found_entry->is_used = false;
        xSemaphoreGive(g_msg_manager.mutex);
    }
}

void msg_manager_unregister_by_id(uint8_t id)
{
    if (g_msg_manager.mutex == NULL) {
        return;
    }

    msg_manager_entry *found_entry = prv_find_entry_by_id(id);
    if (found_entry != NULL) {
        xSemaphoreTake(g_msg_manager.mutex, portMAX_DELAY);
        found_entry->callback = NULL;
        found_entry->is_used = false;
        xSemaphoreGive(g_msg_manager.mutex);
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

    // 设置消息的目标ID（使用type_id字段存储目标ID）
    msg->type_id = to->id;

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
        return (msg_base*)pvPortMalloc(size);
    }

    xSemaphoreTake(msg_pool_mutex, portMAX_DELAY);
    // 查找空闲消息槽
    for (int i = 0; i < MSG_POOL_SIZE; i++) {
        if (!msg_pool[i].is_used) {
            msg_pool[i].is_used = true;
            msg_pool[i].size = size;
            xSemaphoreGive(msg_pool_mutex);
            return (msg_base*)msg_pool[i].buffer;
        }
    }
    xSemaphoreGive(msg_pool_mutex);
    
    // 消息池已满，使用动态分配
    return (msg_base*)pvPortMalloc(size);
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

    xSemaphoreTake(msg_pool_mutex, portMAX_DELAY);
    // 检查是否是从消息池分配的
    for (int i = 0; i < MSG_POOL_SIZE; i++) {
        if (msg_pool[i].is_used && (msg == (msg_base*)msg_pool[i].buffer)) {
            msg_pool[i].is_used = false;
            msg_pool[i].size = 0;
            xSemaphoreGive(msg_pool_mutex);
            return;
        }
    }
    xSemaphoreGive(msg_pool_mutex);
    
    // 不是从消息池分配的，使用动态释放
    if (msg->destroy != NULL) {
        msg->destroy(msg);
    } else {
        vPortFree(msg);
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
        if (msg->type_id == handle->id) {
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