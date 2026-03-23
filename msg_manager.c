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

/**
 * @brief 查找指定名称的队列条目
 *
 * 在管理器中搜索指定名称的队列条目
 *
 * @param name 要查找的队列名称
 * @return 找到的条目指针，未找到返回NULL
 */
static msg_manager_entry *prv_find_entry(const char *name)
{
    if ((name == NULL) || (name[0] == '\0')) {
        return NULL;
    }

    msg_manager_entry *itr = g_msg_manager.head;
    while (itr != NULL) {
        if (strncmp(itr->handle.name, name, MSG_MANAGER_NAME_MAX) == 0) {
            return itr;
        }
        itr = itr->next;
    }

    return NULL;
}

/**
 * @brief 初始化消息管理器
 *
 * 创建全局消息管理器实例和互斥锁
 */
void msg_manager_init(void)
{
    if (g_msg_manager.mutex == NULL) {
        g_msg_manager.head = NULL;
        g_msg_manager.mutex = xSemaphoreCreateMutex();
    }
}

void msg_manager_deinit(void)
{
    if (g_msg_manager.mutex == NULL) {
        return;
    }

    xSemaphoreTake(g_msg_manager.mutex, portMAX_DELAY);

    msg_manager_entry *itr = g_msg_manager.head;
    while (itr != NULL) {
        msg_manager_entry *to_delete = itr;
        itr = itr->next;

        if (to_delete->queue != NULL) {
            msg_queue_destroy(to_delete->queue);
        }

        vPortFree(to_delete);
    }

    g_msg_manager.head = NULL;

    xSemaphoreGive(g_msg_manager.mutex);

    vSemaphoreDelete(g_msg_manager.mutex);
    g_msg_manager.mutex = NULL;
}

/**
 * @brief 检查消息句柄是否有效
 *
 * 验证消息句柄是否有效（非空且名称不为空）
 *
 * @param handle 要检查的句柄
 * @return 有效返回true，无效返回false
 */
bool msg_handle_is_valid(const msg_handle *handle)
{
    return (handle != NULL) && (handle->name[0] != '\0');
}

/**
 * @brief 注册消息队列
 *
 * 将消息队列注册到管理器中，并启动其后台处理任务
 *
 * @param queue 要注册的消息队列
 * @param name 队列名称，用于后续查找
 * @param callback 消息处理回调函数
 * @param empty_event_timeout_ms 队列空事件超时时间(毫秒)，-1表示无超时
 * @return 注册成功返回true，失败返回false
 */
bool msg_manager_register(msg_queue_handle queue,
                        const char *name,
                        msg_callback callback,
                        int empty_event_timeout_ms)
{
    if ((queue == NULL) || (name == NULL) || (name[0] == '\0')) {
        return false;
    }

    if (g_msg_manager.mutex == NULL) {
        msg_manager_init();
    }

    xSemaphoreTake(g_msg_manager.mutex, portMAX_DELAY);

    if (prv_find_entry(name) != NULL) {
        xSemaphoreGive(g_msg_manager.mutex);
        return false;
    }

    msg_manager_entry *entry = pvPortMalloc(sizeof(msg_manager_entry));
    if (entry == NULL) {
        xSemaphoreGive(g_msg_manager.mutex);
        return false;
    }

    entry->queue = queue;
    entry->next = g_msg_manager.head;
    strncpy(entry->handle.name, name, MSG_MANAGER_NAME_MAX - 1);
    entry->handle.name[MSG_MANAGER_NAME_MAX - 1] = '\0';
    g_msg_manager.head = entry;

    msg_queue_set_callback(queue, callback);
    msg_queue_set_get_msg_timeout_ms(queue, empty_event_timeout_ms);
    msg_queue_start(queue);

    xSemaphoreGive(g_msg_manager.mutex);

    return true;
}

void msg_manager_unregister_by_name(const char *name)
{
    if ((name == NULL) || (name[0] == '\0') || (g_msg_manager.mutex == NULL)) {
        return;
    }

    xSemaphoreTake(g_msg_manager.mutex, portMAX_DELAY);

    msg_manager_entry *prev = NULL;
    msg_manager_entry *itr = g_msg_manager.head;

    while (itr != NULL) {
        if (strncmp(itr->handle.name, name, MSG_MANAGER_NAME_MAX) == 0) {
            if (prev == NULL) {
                g_msg_manager.head = itr->next;
            } else {
                prev->next = itr->next;
            }

            msg_queue_destroy(itr->queue);
            vPortFree(itr);
            break;
        }

        prev = itr;
        itr = itr->next;
    }

    xSemaphoreGive(g_msg_manager.mutex);
}

void msg_manager_unregister_by_handle(const msg_handle *handle)
{
    if ((handle == NULL) || (handle->name[0] == '\0')) {
        return;
    }

    msg_manager_unregister_by_name(handle->name);
}

msg_queue_code msg_manager_send_msg(const char *from,
                                const char *to,
                                msg_base *msg)
{
    (void)from; /* 保留参数，当前实现不注入 msg_handle */

    if ((to == NULL) || (to[0] == '\0') || (msg == NULL) || (g_msg_manager.mutex == NULL)) {
        return MSG_QUEUE_CODE_NOT_EXISTS;
    }

    xSemaphoreTake(g_msg_manager.mutex, portMAX_DELAY);

    msg_manager_entry *entry = prv_find_entry(to);
    if (entry == NULL) {
        xSemaphoreGive(g_msg_manager.mutex);
        return MSG_QUEUE_CODE_NOT_EXISTS;
    }

    msg_queue_code status = msg_queue_push(entry->queue, msg);

    xSemaphoreGive(g_msg_manager.mutex);
    return status;
}

msg_queue_code msg_manager_send_msg_to(const char *to, msg_base *msg)
{
    return msg_manager_send_msg(NULL, to, msg);
}

int msg_manager_size(const char *name)
{
    if ((name == NULL) || (name[0] == '\0') || (g_msg_manager.mutex == NULL)) {
        return 0;
    }

    xSemaphoreTake(g_msg_manager.mutex, portMAX_DELAY);

    msg_manager_entry *entry = prv_find_entry(name);
    if (entry == NULL) {
        xSemaphoreGive(g_msg_manager.mutex);
        return 0;
    }

    int size = msg_queue_size(entry->queue);

    xSemaphoreGive(g_msg_manager.mutex);
    return size;
}

