/**
 * @file msg_demo.c
 * @brief 消息管理器演示代码
 *
 * 演示消息管理器的使用方法，包括消息创建、发送和处理
 */

#include "msg_manager.h"
#include "msg_queue.h"
#include <stdio.h>

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
        printf("System: Receive Timeout Event (%d ms)\n", t_msg->timeout_ms);
    } else if (msg->type_id == MSG_TYPE_DATA) {
        my_data_msg* d_msg = (my_data_msg*)msg;
        printf("Data: Sensor %d, Value %.2f\n", d_msg->sensor_id, d_msg->value);
    } else {
        printf("Unknown message type: %d\n", msg->type_id);
    }
    
    // 释放消息
    msg_manager_free_msg(msg);
}

// 演示任务：定期发送消息
void prv_demo_task(void *pv_parameters) {
    (void)pv_parameters;

    // 单队列优化：不再需要创建独立队列
    msg_handle* handle = msg_manager_register(my_process_callback, -1);
    if (handle == NULL) {
        printf("Failed to register message queue\n");
        return;
    }

    static int counter = 0;
    
    for (;;) {
        my_data_msg* msg = my_data_msg_create(3, 20.0 + counter);
        if (msg) {
            printf("DemoTask: Sending message %d\n", counter + 1);
            msg_queue_code result = msg_manager_send_msg_to(handle, (msg_base*)msg);
            if (result != MSG_QUEUE_CODE_OK) {
                printf("Failed to send message, error code: %d\n", result);
                msg_manager_free_msg((msg_base*)msg);
            } else {
                counter++;
            }
        } else {
            printf("Failed to allocate message\n");
        }
        
        if (counter >= 5) {
            // 发送完5个消息后退出
            break;
        }

        // 每5秒发送一个消息
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    // 任务结束
    msg_manager_unregister_by_handle(handle);
    vTaskDelete(NULL);
}

// 初始化消息演示
void msg_demo_init(void) {
    // 初始化消息管理器
    msg_manager_init();
    
    // 创建演示任务
    xTaskCreate(prv_demo_task, "DemoTask", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 1, NULL);
}