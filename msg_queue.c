
#include "msg_queue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// 内部结构定义
// ============================================================================

struct msg_queue_obj {
    QueueHandle_t x_queue;             // FreeRTOS 队列，存储 msg_base*
    TaskHandle_t x_task_handle;         // 后台处理任务
    volatile bool b_stop;              // 停止标志
    
    // 配置参数
    uint32_t ul_max_size;
    int i_get_msg_timeout_ms;             // 接收超时 (生成 timeout_msg)
    int i_push_timeout_ms;               // 发送默认超时
    
    // 回调
    msg_callback pfn_callback;
};

// ============================================================================
// 辅助函数实现
// ============================================================================

// timeout_msg 的 destroy 实现
static void prv_timeout_msg_destroy(msg_base* self) {
    if (self) {
        vPortFree(self);
    }
}

timeout_msg* timeout_msg_create(int ms) {
    timeout_msg* p_msg = (timeout_msg*)pvPortMalloc(sizeof(timeout_msg));
    if (p_msg) {
        p_msg->base.destroy = prv_timeout_msg_destroy;
        p_msg->base.type_id = MSG_TYPE_TIMEOUT;
        p_msg->timeout_ms = ms;
    }
    return p_msg;
}

// ============================================================================
// 后台任务线程 (对应 WaitAndPopLoop 和 ThreadFunc)
// ============================================================================

static void prv_msg_queue_task(void *pv_parameters) {
    msg_queue_obj* p_obj = (msg_queue_obj*)pv_parameters;
    msg_base* px_msg = NULL;
    TickType_t x_ticks_to_wait;
    BaseType_t x_result;

    while (!p_obj->b_stop) {
        // 决定等待时间
        if (p_obj->i_get_msg_timeout_ms > 0) {
            x_ticks_to_wait = pdMS_TO_TICKS(p_obj->i_get_msg_timeout_ms);
        } else {
            x_ticks_to_wait = portMAX_DELAY;
        }

        // 尝试接收消息
        // 如果设置了超时时间，这里会阻塞最多 xTicksToWait
        // 如果没有设置 (<=0)，这里一直阻塞直到有消息
        x_result = xQueueReceive(p_obj->x_queue, &px_msg, x_ticks_to_wait);

        if (p_obj->b_stop) {
            break;
        }

        if (x_result == pdFALSE) {
            // --- 超时发生 ---
            // 逻辑：生成 timeout_msg 并回调
            
            if (p_obj->i_get_msg_timeout_ms > 0) {
                // 检查队列是否真的为空（防止竞争条件，虽然单消费者任务下通常不需要）
                // 创建超时消息
                timeout_msg* p_timeout_msg = timeout_msg_create(p_obj->i_get_msg_timeout_ms);
                
                if (p_timeout_msg != NULL) {
                    if (p_obj->pfn_callback != NULL) {
                        p_obj->pfn_callback((msg_base*)p_timeout_msg);
                    }
                    // 模拟 unique_ptr 离开作用域：调用 destroy 释放内存
                    if (p_timeout_msg->base.destroy) {
                        p_timeout_msg->base.destroy((msg_base*)p_timeout_msg);
                    }
                }
            }
            // 继续循环
            continue;
        }

        // --- 收到有效消息 ---
        // px_msg 现在指向从队列取出的消息指针
        
        if (px_msg != NULL && p_obj->pfn_callback != NULL) {
            p_obj->pfn_callback(px_msg);
            
            // 回调处理后，释放消息内存
            if (px_msg->destroy) {
                px_msg->destroy(px_msg);
            }
        }
        
        // 在 RTOS 中，短暂延时让出 CPU，防止高优先级死循环占用
        vTaskDelay(pdMS_TO_TICKS(1)); 
    }

    vTaskDelete(NULL);
}

// ============================================================================
// API 实现
// ============================================================================

msg_queue_handle msg_queue_create(uint32_t max_size) {
    msg_queue_obj* p_obj = (msg_queue_obj*)pvPortMalloc(sizeof(msg_queue_obj));
    if (p_obj == NULL) {
        return NULL;
    }

    // 创建队列，元素大小为指针大小
    p_obj->x_queue = xQueueCreate(max_size, sizeof(msg_base*));
    if (p_obj->x_queue == NULL) {
        vPortFree(p_obj);
        return NULL;
    }

    p_obj->ul_max_size = max_size;
    p_obj->i_get_msg_timeout_ms = -1; // 默认无超时生成
    p_obj->i_push_timeout_ms = -1;   // 默认阻塞
    p_obj->b_stop = false;
    p_obj->pfn_callback = NULL;
    p_obj->x_task_handle = NULL;

    return p_obj;
}

void msg_queue_destroy(msg_queue_handle h_queue) {
    if (h_queue == NULL) return;

    // 1. 通知线程停止
    h_queue->b_stop = true;

    // 2. 唤醒阻塞在 xQueueReceive 的任务
    // 发送一个空指针或者任意消息让它醒来检查 bStop
    msg_base* dummy = NULL;
    xQueueSend(h_queue->x_queue, &dummy, 0);

    // 等待一小段时间让任务退出 (可选，取决于系统是否允许阻塞在销毁中)
    // 如果任务优先级低，可能需要更长时间，或者直接 vTaskDelete 强制杀
    // 这里假设任务能迅速响应 bStop
    int wait_count = 0;
    while (h_queue->x_task_handle != NULL && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(1));
        wait_count++;
    }
    
    // 如果任务还没退出的极端情况，强制删除 (慎用，但在销毁场景可接受)
    if (h_queue->x_task_handle != NULL) {
        vTaskDelete(h_queue->x_task_handle);
        h_queue->x_task_handle = NULL;
    }

    // 3. 清理队列中剩余的消息 (对应 C++ 析构中的遍历销毁)
    msg_base* px_msg;
    while (xQueueReceive(h_queue->x_queue, &px_msg, 0) == pdTRUE) {
        if (px_msg != NULL && px_msg->destroy != NULL) {
            px_msg->destroy(px_msg);
        }
    }

    // 4. 删除 FreeRTOS 对象
    vQueueDelete(h_queue->x_queue);
    vPortFree(h_queue);
}

void msg_queue_start(msg_queue_handle h_queue) {
    if (h_queue == NULL || h_queue->x_task_handle != NULL) return;

    // 创建任务
    // 栈大小需要根据 msg_base 派生类的大小和调用栈深度调整
    // 优先级设为比普通空闲任务高，但可根据需求调整
    BaseType_t ret = xTaskCreate(prv_msg_queue_task, 
                                 "MsgQ_Task", 
                                 configMINIMAL_STACK_SIZE * 4, 
                                 (void*)h_queue, 
                                 tskIDLE_PRIORITY + 2, 
                                 &h_queue->x_task_handle);
    
    if (ret != pdPASS) {
        h_queue->x_task_handle = NULL;
        // 错误处理
    }
}

void msg_queue_stop(msg_queue_handle h_queue) {
    if (h_queue) h_queue->b_stop = true;
}

// 内部辅助：尝试推送 (对应 TryPush)
static msg_queue_code prv_try_push_internal(msg_queue_handle h_queue, msg_base* msg) {
    if (uxQueueSpacesAvailable(h_queue->x_queue) == 0) {
        return MSG_QUEUE_CODE_REACH_MAX_SIZE;
    }
    
    // 尝试发送，不阻塞
    if (xQueueSend(h_queue->x_queue, &msg, 0) == pdTRUE) {
        return MSG_QUEUE_CODE_OK;
    } else {
        // 理论上上面检查过空间，这里失败可能是并发竞争或其他错误
        return MSG_QUEUE_CODE_LOCK_FAILED;
    }
}

msg_queue_code msg_queue_try_push(msg_queue_handle h_queue, msg_base* msg) {
    if (h_queue == NULL || msg == NULL) return MSG_QUEUE_CODE_ERROR;
    return prv_try_push_internal(h_queue, msg);
}

msg_queue_code msg_queue_push_block(msg_queue_handle h_queue, msg_base* msg) {
    if (h_queue == NULL || msg == NULL) return MSG_QUEUE_CODE_ERROR;
    
    // 阻塞发送，直到成功
    if (xQueueSend(h_queue->x_queue, &msg, portMAX_DELAY) == pdTRUE) {
        return MSG_QUEUE_CODE_OK;
    }
    return MSG_QUEUE_CODE_ERROR;
}

msg_queue_code msg_queue_push_with_timeout(msg_queue_handle h_queue, msg_base* msg, int timeout_ms) {
    if (h_queue == NULL || msg == NULL) return MSG_QUEUE_CODE_ERROR;
    
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    
    if (xQueueSend(h_queue->x_queue, &msg, ticks) == pdTRUE) {
        return MSG_QUEUE_CODE_OK;
    }
    
    // 超时
    return MSG_QUEUE_CODE_LOCK_FAILED; // 或定义 TIMEOUT 码
}

msg_queue_code msg_queue_push(msg_queue_handle h_queue, msg_base* msg) {
    if (h_queue == NULL || msg == NULL) return MSG_QUEUE_CODE_ERROR;

    if (h_queue->i_push_timeout_ms > 0) {
        return msg_queue_push_with_timeout(h_queue, msg, h_queue->i_push_timeout_ms);
    } else {
        // 如果 <= 0，默认为阻塞
        return msg_queue_push_block(h_queue, msg);
    }
}

msg_queue_code msg_queue_pop(msg_queue_handle h_queue, msg_base** out_msg, pop_type pop_type) {
    if (h_queue == NULL || out_msg == NULL) return MSG_QUEUE_CODE_ERROR;

    TickType_t ticks = (pop_type == POP_BLOCK) ? portMAX_DELAY : 0;

    if (xQueueReceive(h_queue->x_queue, out_msg, ticks) == pdTRUE) {
        return MSG_QUEUE_CODE_OK;
    } else {
        return MSG_QUEUE_CODE_EMPTY;
    }
}

void msg_queue_set_callback(msg_queue_handle h_queue, msg_callback cb) {
    if (h_queue) h_queue->pfn_callback = cb;
}

void msg_queue_set_get_msg_timeout_ms(msg_queue_handle h_queue, int ms) {
    if (h_queue) h_queue->i_get_msg_timeout_ms = ms;
}

void msg_queue_set_push_timeout_ms(msg_queue_handle h_queue, int ms) {
    if (h_queue) h_queue->i_push_timeout_ms = ms;
}

int msg_queue_size(msg_queue_handle h_queue) {
    if (h_queue == NULL) return 0;
    return (int)uxQueueMessagesWaiting(h_queue->x_queue);
}