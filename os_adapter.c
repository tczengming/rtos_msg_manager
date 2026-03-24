/**
 * @file os_adapter.c
 * @brief OS适配器实现（FreeRTOS版本）
 *
 * 封装FreeRTOS的底层接口，提供统一的API
 */

#include "os_adapter.h"
#include <stdarg.h>

// 任务管理
os_task_handle os_task_create(const char* name, os_task_func func, void* param, uint32_t stack_size, os_task_priority_t priority) {
    TaskHandle_t task_handle;
    BaseType_t result = xTaskCreate(func, name, stack_size, param, priority, &task_handle);
    return (result == pdPASS) ? (os_task_handle)task_handle : NULL;
}

os_task_handle os_task_create_static(const char* name, os_task_func func, void* param, uint32_t stack_size, os_task_priority_t priority, void* stack_buffer, void* task_buffer) {
    return (os_task_handle)xTaskCreateStatic(func, name, stack_size, param, priority, (StackType_t*)stack_buffer, (StaticTask_t*)task_buffer);
}

void os_task_delete(os_task_handle task) {
    vTaskDelete((TaskHandle_t)task);
}

// 互斥锁
os_semaphore_handle os_semaphore_create_mutex(void) {
    return (os_semaphore_handle)xSemaphoreCreateMutex();
}

os_semaphore_handle os_semaphore_create_mutex_static(void* buffer) {
    return (os_semaphore_handle)xSemaphoreCreateMutexStatic((StaticSemaphore_t*)buffer);
}

// 定时器
os_timer_handle os_timer_create(const char* name, os_tick_t period, bool auto_reload, void* param, os_timer_func func) {
    return (os_timer_handle)xTimerCreate(name, period, auto_reload, param, (TimerCallbackFunction_t)func);
}

// 队列
os_queue_handle os_queue_create(uint32_t queue_length, uint32_t item_size) {
    return (os_queue_handle)xQueueCreate(queue_length, item_size);
}

os_queue_handle os_queue_create_static(uint32_t queue_length, uint32_t item_size, void* buffer, void* queue_buffer) {
    return (os_queue_handle)xQueueCreateStatic(queue_length, item_size, (uint8_t*)buffer, (StaticQueue_t*)queue_buffer);
}