/**
 * @file msg_demo.c
 * @brief 消息管理器演示代码
 *
 * 演示消息管理器的使用方法，包括消息创建、发送和处理
 */

#include "msg_manager.h"
#include "msg_queue.h"
#include "os_adapter.h"

/** 队列ID枚举定义 */
typedef enum {
    MSG_QUEUE_ID_NORMAL = 1,    /**< 正常消息队列ID */
    MSG_QUEUE_ID_BLOCKING,       /**< 阻塞消息队列ID */
    MSG_QUEUE_ID_MAX             /**< 最大队列ID，用于边界检查 */
} msg_queue_id_t;

// 1. 定义消息结构
typedef struct my_data_msg {
    msg_base base; // 必须是第一个成员，方便强转
    int sensor_id;
    float value;
} my_data_msg;

// 2. 实现 destroy 函数
void my_data_msg_destroy(msg_base* self) {
    my_data_msg* p = (my_data_msg*)self;
    // 如果消息里面有额外的指针动态分配了内存，要在这里 free
    // 否则这里不要free,消息池会自动处理内存管理
}

// 3. 工厂函数 (替代 new/make_unique)
my_data_msg* my_data_msg_create(int id, float val) {
    my_data_msg* p = (my_data_msg*)msg_manager_alloc_msg(sizeof(my_data_msg));
    if (p) {
        p->base.destroy = my_data_msg_destroy;
        p->base.type_id = MSG_TYPE_DATA;
        p->base.callback = NULL; // 默认为NULL，使用队列级回调
        p->sensor_id = id;
        p->value = val;
    }
    return p;
}

// 3.1 工厂函数（带回调）
my_data_msg* my_data_msg_create_with_callback(int id, float val, void (*callback)(msg_base*)) {
    my_data_msg* p = (my_data_msg*)msg_manager_alloc_msg(sizeof(my_data_msg));
    if (p) {
        p->base.destroy = my_data_msg_destroy;
        p->base.type_id = MSG_TYPE_DATA;
        p->base.callback = callback; // 设置消息级回调
        p->sensor_id = id;
        p->value = val;
    }
    return p;
}

// 4. 回调函数
void my_process_callback(msg_base* msg) {
    if (!msg) return;

    if (msg->type_id == MSG_TYPE_TIMEOUT) {
        timeout_msg* t_msg = (timeout_msg*)msg;
        os_log("System: Receive Timeout Event (%d ms)", t_msg->timeout_ms);
    } else if (msg->type_id == MSG_TYPE_DATA) {
        my_data_msg* d_msg = (my_data_msg*)msg;
        os_log("Data: Sensor %d, Value %.2f", d_msg->sensor_id, d_msg->value);
    } else {
        os_log("Unknown message type: %d", msg->type_id);
    }
    
    // 不需要手动释放消息，分发器会自动释放
}

// 5. 故意阻塞的回调函数（用于测试超时机制）
void my_blocking_callback(msg_base* msg) {
    if (!msg) return;

    if (msg->type_id == MSG_TYPE_DATA) {
        my_data_msg* d_msg = (my_data_msg*)msg;
        os_log("Blocking callback: Sensor %d, Value %.2f", d_msg->sensor_id, d_msg->value);
        
        // 故意阻塞2秒，超过1秒的超时时间
        os_log("Blocking callback: Starting to block...");
        os_task_delay(os_ms_to_ticks(2000));
        os_log("Blocking callback: Blocking completed");
    }
    
    // 不需要手动释放消息，分发器会自动释放
}

// 演示任务：定期发送消息
void prv_demo_task(void *pv_parameters) {
    (void)pv_parameters;

    // 使用指定的队列ID注册队列
    msg_handle* normal_handle = msg_manager_register_with_id(MSG_QUEUE_ID_NORMAL, my_process_callback, -1);
    if (normal_handle == NULL) {
        os_log("Failed to register normal message queue");
        return;
    }

    // 注册一个使用阻塞回调的队列（用于测试超时机制）
    msg_handle* blocking_handle = msg_manager_register_with_id(MSG_QUEUE_ID_BLOCKING, my_blocking_callback, -1);
    if (blocking_handle == NULL) {
        os_log("Failed to register blocking message queue");
        msg_manager_unregister_by_id(MSG_QUEUE_ID_NORMAL);
        return;
    }

    static int counter = 0;
    
    for (;;) {
        // 发送正常消息
        my_data_msg* normal_msg = my_data_msg_create(1, 10.0 + counter);
        if (normal_msg) {
            os_log("DemoTask: Sending normal message %d", counter + 1);
            msg_queue_code normal_result = msg_manager_send_msg_to_id(MSG_QUEUE_ID_NORMAL, (msg_base*)normal_msg);
            if (normal_result != MSG_QUEUE_CODE_OK) {
                os_log("Failed to send normal message, error code: %d", normal_result);
                msg_manager_free_msg((msg_base*)normal_msg);
            }
        } else {
            os_log("Failed to allocate normal message");
        }

        // 发送阻塞消息（测试超时机制）
        my_data_msg* blocking_msg = my_data_msg_create(2, 20.0 + counter);
        if (blocking_msg) {
            os_log("DemoTask: Sending blocking message %d", counter + 1);
            msg_queue_code blocking_result = msg_manager_send_msg_to_id(MSG_QUEUE_ID_BLOCKING, (msg_base*)blocking_msg);
            if (blocking_result != MSG_QUEUE_CODE_OK) {
                os_log("Failed to send blocking message, error code: %d", blocking_result);
                msg_manager_free_msg((msg_base*)blocking_msg);
            }
        } else {
            os_log("Failed to allocate blocking message");
        }

        counter++;
        
        /*
        if (counter >= 3) {
            os_task_delay(os_ms_to_ticks(5000));
            os_log("发送完3组消息后退出");
            break;
        }
        */

        // 每5秒发送一组消息
        os_task_delay(os_ms_to_ticks(5000));
    }
    
    // 任务结束
    msg_manager_unregister_by_id(MSG_QUEUE_ID_NORMAL);
    msg_manager_unregister_by_id(MSG_QUEUE_ID_BLOCKING);
    os_task_delete(NULL);
}

// 初始化消息演示
void msg_demo_init(void) {
    // 初始化消息管理器
    msg_manager_init();
    
    // 创建演示任务
    os_task_create("DemoTask", prv_demo_task, NULL, OS_MINIMAL_STACK_SIZE * 2, OS_TASK_PRIORITY_NORMAL);
}
