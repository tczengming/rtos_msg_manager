/**
 * @file os_adapter.h
 * @brief OS适配器接口
 *
 * 封装不同OS的底层接口，提供统一的API，方便移植到不同的RTOS或Linux系统
 */

#ifndef OS_ADAPTER_H
#define OS_ADAPTER_H

// 首先包含FreeRTOS的核心头文件
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "timers.h"
#include "queue.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

// 类型定义
typedef TaskHandle_t os_task_handle;
typedef SemaphoreHandle_t os_semaphore_handle;
typedef TimerHandle_t os_timer_handle;
typedef QueueHandle_t os_queue_handle;
typedef TickType_t os_tick_t;
typedef void (*os_task_func)(void *);
typedef TimerCallbackFunction_t os_timer_func;

// 任务优先级
typedef enum {
    OS_TASK_PRIORITY_IDLE = 0,
    OS_TASK_PRIORITY_LOW,
    OS_TASK_PRIORITY_NORMAL,
    OS_TASK_PRIORITY_HIGH,
    OS_TASK_PRIORITY_MAX
} os_task_priority_t;

// 超时时间
typedef enum {
    OS_NO_WAIT = 0,
    OS_WAIT_FOREVER = portMAX_DELAY
} os_wait_t;

// 内存分配和释放
static inline void* os_malloc(size_t size) { return pvPortMalloc(size); }
static inline void os_free(void* ptr) { vPortFree(ptr); }

// 任务管理
os_task_handle os_task_create(const char* name, os_task_func func, void* param, uint32_t stack_size, os_task_priority_t priority);
os_task_handle os_task_create_static(const char* name, os_task_func func, void* param, uint32_t stack_size, os_task_priority_t priority, void* stack_buffer, void* task_buffer);
void os_task_delete(os_task_handle task);
static inline void os_task_delay(os_tick_t ticks) { vTaskDelay(ticks); }
static inline os_tick_t os_ms_to_ticks(uint32_t ms) { return pdMS_TO_TICKS(ms); }

// 任务通知
static inline void os_task_notify_give(os_task_handle task) { xTaskNotifyGive((TaskHandle_t)task); }
static inline os_tick_t os_task_notify_take(bool clear_on_exit, os_tick_t timeout) { return ulTaskNotifyTake(clear_on_exit, timeout); }

// 互斥锁
os_semaphore_handle os_semaphore_create_mutex(void);
os_semaphore_handle os_semaphore_create_mutex_static(void* buffer);
static inline bool os_semaphore_take(os_semaphore_handle semaphore, os_tick_t timeout) { return xSemaphoreTake((SemaphoreHandle_t)semaphore, timeout) == pdPASS; }
static inline bool os_semaphore_give(os_semaphore_handle semaphore) { return xSemaphoreGive((SemaphoreHandle_t)semaphore) == pdPASS; }

// 定时器
os_timer_handle os_timer_create(const char* name, os_tick_t period, bool auto_reload, void* param, os_timer_func func);
static inline bool os_timer_start(os_timer_handle timer, os_tick_t timeout) { return xTimerStart((TimerHandle_t)timer, timeout) == pdPASS; }
static inline bool os_timer_stop(os_timer_handle timer, os_tick_t timeout) { return xTimerStop((TimerHandle_t)timer, timeout) == pdPASS; }
static inline void* os_timer_get_id(os_timer_handle timer) { return pvTimerGetTimerID((TimerHandle_t)timer); }

// 队列
os_queue_handle os_queue_create(uint32_t queue_length, uint32_t item_size);
os_queue_handle os_queue_create_static(uint32_t queue_length, uint32_t item_size, void* buffer, void* queue_buffer);
static inline bool os_queue_send(os_queue_handle queue, const void* item, os_tick_t timeout) { return xQueueSend((QueueHandle_t)queue, item, timeout) == pdPASS; }
static inline bool os_queue_receive(os_queue_handle queue, void* item, os_tick_t timeout) { return xQueueReceive((QueueHandle_t)queue, item, timeout) == pdPASS; }
static inline uint32_t os_queue_messages_waiting(os_queue_handle queue) { return (uint32_t)uxQueueMessagesWaiting((QueueHandle_t)queue); }
static inline uint32_t os_queue_spaces_available(os_queue_handle queue) { return (uint32_t)uxQueueSpacesAvailable((QueueHandle_t)queue); }

// 日志
#define os_log(format, ...) printf(format "\n", ##__VA_ARGS__); fflush(stdout)

#endif /* OS_ADAPTER_H */