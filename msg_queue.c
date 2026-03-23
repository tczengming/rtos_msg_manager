
#include "msg_queue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// 内部结构定义
// ============================================================================

// msg_queue_obj 结构体已在头文件中定义

// ============================================================================
// 辅助函数实现
// ============================================================================

// timeout_msg 的 destroy 实现
static void prv_timeout_msg_destroy(msg_base* self) {
    if (self) {
        vPortFree(self);
    }
}

timeout_msg* timeout_msg_create(int16_t ms) {
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



// ============================================================================
// API 实现
// ============================================================================

msg_queue_handle msg_queue_create(uint32_t max_size) {
    msg_queue_obj* p_obj = (msg_queue_obj*)pvPortMalloc(sizeof(msg_queue_obj));
    if (p_obj == NULL) {
        return NULL;
    }

    // 限制最大大小以适应静态缓冲区
    if (max_size > MSG_QUEUE_STORAGE_SIZE) {
        max_size = MSG_QUEUE_STORAGE_SIZE;
    }

    // 创建静态队列
    p_obj->x_queue = xQueueCreateStatic(max_size, sizeof(msg_base*),
                                       p_obj->queue_storage, &p_obj->x_queue_buffer);
    if (p_obj->x_queue == NULL) {
        vPortFree(p_obj);
        return NULL;
    }

    p_obj->ul_max_size = max_size;
    p_obj->i_get_msg_timeout_ms = -1; // 默认无超时生成
    p_obj->i_push_timeout_ms = -1;   // 默认阻塞
    p_obj->b_stop = false;
    p_obj->b_static = false;         // 动态创建
    p_obj->pfn_callback = NULL;

    return p_obj;
}

msg_queue_handle msg_queue_create_static(uint32_t max_size) {

    static msg_queue_obj static_queue_obj = {0};

    // 限制最大大小以适应静态缓冲区
    if (max_size > MSG_QUEUE_MAX_ITEMS) {
        max_size = MSG_QUEUE_MAX_ITEMS;
    }

    // 创建静态队列
    static_queue_obj.x_queue = xQueueCreateStatic(max_size, sizeof(msg_base*),
                                                 static_queue_obj.queue_storage,
                                                 &static_queue_obj.x_queue_buffer);
    if (static_queue_obj.x_queue == NULL) {
        return NULL;
    }

    static_queue_obj.ul_max_size = max_size;
    static_queue_obj.i_get_msg_timeout_ms = -1; // 默认无超时生成
    static_queue_obj.i_push_timeout_ms = -1;   // 默认阻塞
    static_queue_obj.b_stop = false;
    static_queue_obj.b_static = true;          // 静态创建
    static_queue_obj.pfn_callback = NULL;

    return &static_queue_obj;
}

void msg_queue_destroy(msg_queue_handle h_queue) {
    if (h_queue == NULL) return;



    // 3. 清理队列中剩余的消息
    msg_base* px_msg;
    while (xQueueReceive(h_queue->x_queue, &px_msg, 0) == pdTRUE) {
        if (px_msg != NULL && px_msg->destroy != NULL) {
            px_msg->destroy(px_msg);
        }
    }

    // 4. 根据创建方式处理清理
    if (h_queue->b_static) {
        // 静态对象：只重置状态，不释放内存
        h_queue->b_stop = false;
        h_queue->pfn_callback = NULL;
    } else {
        // 动态对象：释放结构体内存
        vPortFree(h_queue);
    }
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

msg_queue_code msg_queue_push_with_timeout(msg_queue_handle h_queue, msg_base* msg, int16_t timeout_ms) {
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