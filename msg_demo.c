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

// 5. 故意阻塞的回调函数（用于测试超时机制）
void my_blocking_callback(msg_base* msg) {
    if (!msg) return;

    if (msg->type_id == MSG_TYPE_DATA) {
        my_data_msg* d_msg = (my_data_msg*)msg;
        printf("Blocking callback: Sensor %d, Value %.2f\n", d_msg->sensor_id, d_msg->value);
        
        // 故意阻塞2秒，超过1秒的超时时间
        printf("Blocking callback: Starting to block...\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
        printf("Blocking callback: Blocking completed\n");
    }
    
    // 释放消息
    msg_manager_free_msg(msg);
}

// 演示任务：定期发送消息
void prv_demo_task(void *pv_parameters) {
    (void)pv_parameters;

    msg_handle* normal_handle = msg_manager_register(my_process_callback, -1);
    if (normal_handle == NULL) {
        printf("Failed to register normal message queue\n");
        return;
    }

    // 注册一个使用阻塞回调的队列（用于测试超时机制）
    msg_handle* blocking_handle = msg_manager_register(my_blocking_callback, -1);
    if (blocking_handle == NULL) {
        printf("Failed to register blocking message queue\n");
        msg_manager_unregister_by_handle(normal_handle);
        return;
    }

    static int counter = 0;
    
    for (;;) {
        // 发送正常消息
        my_data_msg* normal_msg = my_data_msg_create(1, 10.0 + counter);
        if (normal_msg) {
            printf("DemoTask: Sending normal message %d\n", counter + 1);
            msg_queue_code normal_result = msg_manager_send_msg_to(normal_handle, (msg_base*)normal_msg);
            if (normal_result != MSG_QUEUE_CODE_OK) {
                printf("Failed to send normal message, error code: %d\n", normal_result);
                msg_manager_free_msg((msg_base*)normal_msg);
            }
        } else {
            printf("Failed to allocate normal message\n");
        }

        // 发送阻塞消息（测试超时机制）
        my_data_msg* blocking_msg = my_data_msg_create(2, 20.0 + counter);
        if (blocking_msg) {
            printf("DemoTask: Sending blocking message %d\n", counter + 1);
            msg_queue_code blocking_result = msg_manager_send_msg_to(blocking_handle, (msg_base*)blocking_msg);
            if (blocking_result != MSG_QUEUE_CODE_OK) {
                printf("Failed to send blocking message, error code: %d\n", blocking_result);
                msg_manager_free_msg((msg_base*)blocking_msg);
            }
        } else {
            printf("Failed to allocate blocking message\n");
        }

        counter++;
        
        if (counter >= 3) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            printf("发送完3组消息后退出\n");
            break;
        }

        // 每5秒发送一组消息
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    // 任务结束
    msg_manager_unregister_by_handle(normal_handle);
    msg_manager_unregister_by_handle(blocking_handle);
    vTaskDelete(NULL);
}

// 初始化消息演示
void msg_demo_init(void) {
    // 初始化消息管理器
    msg_manager_init();
    
    // 创建演示任务
    xTaskCreate(prv_demo_task, "DemoTask", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 1, NULL);
}