# Message Manager (msg_manager) User Guide

## 1. Overview

Message Manager is a lightweight message passing system based on FreeRTOS, providing thread-safe message sending and receiving mechanisms, with support for callback timeout detection and interrupt handling.

## 2. Features

- Thread-safe message sending and receiving
- Support for callback functions to process messages
- Optional callback timeout detection mechanism
- Optional callback interrupt handling mechanism
- Message pool management for optimized memory usage
- Support for static memory allocation, no dynamic memory required

## 3. Key Macro Definitions

### 3.1 ENABLE_CALLBACK_TIMEOUT

**Function**: Enable callback timeout detection mechanism

**Purpose**:
- When the callback function execution time exceeds the set timeout time, it will detect the timeout and issue a reminder
- Prevent a single callback function from executing for too long, affecting the system's response performance
- Improve system reliability and stability

**RAM Usage Increase**:
- **Task pool**: Each task occupies about 256 bytes of stack space, default 2 tasks
- Timer: Each task has one timer, occupying about 64 bytes
- Total increase: About 640 bytes

**ROM Usage Increase**:
- Task pool management code: About 1.5 KB
- Timeout detection code: About 0.5 KB
- Total increase: About 2 KB

### 3.2 ENABLE_CALLBACK_INTERRUPT

- **Function**: Enable the real interrupt blocking callback function, when the callback function execution times out, it will forcibly terminate the execution of the callback function through interrupt.
- **Default value**: Undefined (disabled)
- **Usage scenario**: When the callback function may have an infinite loop or serious blocking, it is necessary to forcibly terminate the execution of the callback function.
- **Dependency**: Needs to be used together with ENABLE_CALLBACK_TIMEOUT, invalid when used alone.

### 3.3 ENABLE_DYNAMIC_TASK_POOL

**Function**: Enable dynamic task pool size adjustment functionality

**Effects**:
- Dynamically adjust task pool size based on system load, optimizing system resource usage
- Automatically increase task pool size when load is high
- Automatically decrease task pool size when load is low
- Avoid resource waste caused by overly large task pool or task waiting caused by overly small task pool

**Configuration parameters**:
- `MIN_TASK_POOL_SIZE`: Minimum task pool size, default 2 (must be 2 or greater, otherwise timeout abort functionality will fail)
- `MAX_TASK_POOL_SIZE`: Maximum task pool size, default 4
- `DEFAULT_TASK_POOL_SIZE`: Default task pool size, default 2
- `TASK_POOL_ADJUST_INTERVAL_MS`: Task pool adjustment interval, default 10000ms
- `TASK_POOL_COOLDOWN_PERIOD_MS`: Task pool adjustment cooldown period, default 60000ms
- `TASK_POOL_LOAD_THRESHOLD`: Task pool load threshold, default 80%

**Important note**:
- The minimum task pool size must be set to 2 or greater, otherwise timeout abort functionality will fail
- This is because when one task is executing a callback, another task is needed to handle timeout detection and abort operations

**RAM usage impact**:
- Task pool size will be dynamically adjusted based on load, range: 2-4 tasks
- Each task occupies about 256 bytes of stack space
- Each task has one timer, occupying about 64 bytes
- RAM usage will change based on current task pool size during dynamic adjustment

**ROM usage increase**:
- Dynamic task pool management code: about 1 KB
- Load calculation and adjustment logic: about 0.5 KB
- Total increase: about 1.5 KB

### 3.4 Message-level callbacks

- **Function**: Support setting callback functions in messages. When a message is processed, it will preferentially execute the message-level callback instead of the queue-level callback.
- **Usage scenario**: When different messages require different processing logic, using message-level callbacks can avoid a lot of type judgment in queue callbacks.

## 4. Usage

### 4.1 Initialize Message Manager

```c
#include "msg_manager.h"

void app_init(void) {
    // Initialize message manager
    msg_manager_init();
}
```

### 4.2 Define Message Structure

```c
// Define message structure
typedef struct my_data_msg {
    msg_base base; // Must be the first member for easy casting
    int sensor_id;
    float value;
} my_data_msg;

// Implement destroy function
void my_data_msg_destroy(msg_base* self) {
    my_data_msg* p = (my_data_msg*)self;
    // If the message contains additional pointers that dynamically allocate memory, free them here
    // Otherwise, don't free here, the message pool will handle memory management automatically
}

// Factory function (replaces new/make_unique)
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
```

### 4.3 Register Message Processing Callback

```c
// Callback function
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
    
    // Free message
    msg_manager_free_msg(msg);
}

// Register message queue
msg_handle* normal_handle = msg_manager_register(my_process_callback, -1);
```

### 4.4 Send Message

#### Send Message by Handle

```c
// Create message
my_data_msg* msg = my_data_msg_create(1, 10.0);

// Send message
msg_queue_code result = msg_manager_send_msg_to(normal_handle, (msg_base*)msg);
if (result != MSG_QUEUE_CODE_OK) {
    os_log("Failed to send message, error code: %d", result);
    msg_manager_free_msg((msg_base*)msg);
}
```

#### Send Message by Queue ID

```c
// Get queue ID
uint8_t queue_id = normal_handle->id;

// Create message
my_data_msg* msg = my_data_msg_create(1, 10.0);

// Send message by ID
msg_queue_code result = msg_manager_send_msg_to_id(queue_id, (msg_base*)msg);
if (result != MSG_QUEUE_CODE_OK) {
    os_log("Failed to send message, error code: %d", result);
    msg_manager_free_msg((msg_base*)msg);
}
```

#### Use Enum to Identify Queue ID

```c
// 1. Queue ID enum definition (defined in msg_demo.c)
typedef enum {
    MSG_QUEUE_ID_NORMAL = 1,    // Normal message queue ID
    MSG_QUEUE_ID_BLOCKING,       // Blocking message queue ID
    MSG_QUEUE_ID_MAX             // Max queue ID, used for boundary checking
} msg_queue_id_t;

// 2. Register queue with specified ID
msg_handle* normal_handle = msg_manager_register_with_id(MSG_QUEUE_ID_NORMAL, my_process_callback, -1);
msg_handle* blocking_handle = msg_manager_register_with_id(MSG_QUEUE_ID_BLOCKING, my_blocking_callback, -1);

// 3. Send message using enum value directly in other tasks
my_data_msg* msg = my_data_msg_create(1, 10.0);
msg_queue_code result = msg_manager_send_msg_to_id(MSG_QUEUE_ID_NORMAL, (msg_base*)msg);
if (result != MSG_QUEUE_CODE_OK) {
    os_log("Failed to send message, error code: %d", result);
    msg_manager_free_msg((msg_base*)msg);
}
```

#### Use Message-Level Callback

```c
// 1. Define message-level callback function
void my_special_callback(msg_base* msg) {
    if (!msg) return;
    
    my_data_msg* d_msg = (my_data_msg*)msg;
    os_log("Special handling: Sensor %d, Value %.2f", d_msg->sensor_id, d_msg->value);
    
    // No need to manually free the message, dispatcher will automatically free it
}

// 2. Create message with callback
my_data_msg* msg = my_data_msg_create_with_callback(1, 10.0, my_special_callback);

// 3. Send message
msg_queue_code result = msg_manager_send_msg_to_id(MSG_QUEUE_ID_NORMAL, (msg_base*)msg);
if (result != MSG_QUEUE_CODE_OK) {
    os_log("Failed to send message, error code: %d", result);
    msg_manager_free_msg((msg_base*)msg);
}
```

### 4.5 Automatic Release Mechanism

The message manager implements an automatic release mechanism with the following features:

1. **Automatic Release**: The message dispatcher automatically calls `msg_manager_free_msg` to release the message after processing it. Callback functions do not need to manually release messages.

2. **Callback Priority**:
   - Message-level callback (`msg->callback`) is used first
   - If message-level callback is NULL, queue-level callback (the callback specified when registering the queue) is used

3. **Memory Management**:
   - The message pool automatically manages message memory allocation and release
   - For messages larger than the message pool size, dynamic memory allocation is used

4. **Usage Recommendations**:
   - Do not manually call `msg_manager_free_msg` in callback functions, as this will cause double release
   - Use message-level callback for messages that require special handling
   - Use queue-level callback for message types that require unified handling

5. **Backward Compatibility**:
   - Existing code using queue-level callback does not need to be modified
   - New code can choose to use message-level callback or queue-level callback


### 4.5 Unregister Message Queue

```c
// Unregister by handle
msg_manager_unregister_by_handle(normal_handle);

// Unregister by ID
msg_manager_unregister_by_id(handle->id);
```

## 5. Porting Guide

### 5.1 Dependencies

- FreeRTOS or other supported RTOS
- C standard library
- OS adapter layer

### 5.2 Porting Steps

1. **Modify OS Adapter**:
   - Implement the target system's interface in `os_adapter.h` and `os_adapter.c`
   - Ensure all necessary functions are implemented

2. **Configure Memory Management**:
   - Modify `os_malloc` and `os_free` functions according to the target system's memory management method

3. **Configure Task Management**:
   - Modify task creation, deletion, delay and other functions according to the target system's task management method

4. **Configure Queue Management**:
   - Modify queue creation, sending, receiving and other functions according to the target system's queue management method

5. **Configure Timer**:
   - Modify timer creation, start, stop and other functions according to the target system's timer management method

6. **Configure Mutex**:
   - Modify mutex creation, take, give and other functions according to the target system's mutex management method

7. **Configure Log**:
   - Modify `os_log` function according to the target system's log system

### 5.3 Porting Example

#### Porting to Linux System

```c
// Modify os_adapter.h
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>

// Type definitions
typedef pthread_t os_task_handle;
typedef sem_t os_semaphore_handle;
typedef timer_t os_timer_handle;
typedef void* os_queue_handle;
typedef uint32_t os_tick_t;
typedef void (*os_task_func)(void *);
typedef void (*os_timer_func)(void *);

// Implement os_adapter.c
void* os_malloc(size_t size) {
    return malloc(size);
}

void os_free(void* ptr) {
    free(ptr);
}

// Other function implementations...
```

## 6. RAM and ROM Usage Analysis

### 6.1 Basic Functionality (Default Configuration)

- **RAM Usage**: About 1 KB
  - Message manager structure: About 256 bytes
  - Message pool: About 512 bytes (MSG_POOL_SIZE=10, 64 bytes per message item)
  - Queue buffer: About 256 bytes (MSG_QUEUE_MAX_ITEMS=20, 4 bytes per message pointer)
  - **Task stack**: Not included (configured by user)

- **ROM Usage**: About 3.7 KB
  - msg_manager.c: About 2.7 KB (including the new msg_manager_send_msg_to_id and msg_manager_register_with_id functions)
  - msg_queue.c: About 1 KB
  - Does not include code from other files like main.c

### 6.2 Basic Task Stack Size

- **Basic task stack size**:
  - Message dispatcher task (including callback processing): 128 words (512 bytes)
  - Demo task: Not included in basic stack size (user configurable)

- **Stack size configuration**:
  - Message dispatcher task stack size is defined by `MSG_DISPATCHER_STACK_SIZE` macro
  - Callback task stack size is defined by `configMINIMAL_STACK_SIZE * 2` (used when ENABLE_CALLBACK_TIMEOUT is enabled)
  - Demo task stack size is defined by `configMINIMAL_STACK_SIZE * 2` (user configurable)

### 6.3 Enable ENABLE_CALLBACK_TIMEOUT

- **RAM Usage Increase**: About 1280 bytes
  - Task pool data structure: About 256 bytes
  - Timer: 4 timers × 64 bytes = 256 bytes
  - **Task stack**: 4 callback tasks × 256 bytes = 1024 bytes (calculated separately)

- **ROM Usage Increase**: About 1.5 KB
  - Task pool management code: About 1 KB
  - Timeout detection code: About 0.5 KB

### 6.4 Enable ENABLE_CALLBACK_INTERRUPT

If you need to use the real interrupt blocking callback function, you can uncomment the `ENABLE_CALLBACK_INTERRUPT` macro definition in `msg_manager.h`:

```c
// Define this macro to enable the real interrupt blocking callback function
#define ENABLE_CALLBACK_INTERRUPT
```

This way, when the callback function execution times out, the system will forcibly terminate the execution of the callback function through interrupt, ensuring that message processing will not be blocked for a long time.

**Note**: ENABLE_CALLBACK_INTERRUPT only adds a small amount of code, and its impact on RAM and ROM usage is minimal, almost negligible.

- **RAM Usage Increase**: About 128 bytes
  - Interrupt-related data structures: About 128 bytes

- **ROM Usage Increase**: About 0.8 KB
  - Interrupt handling code: About 0.8 KB

**Explanation**: After enabling ENABLE_CALLBACK_INTERRUPT, ROM usage will increase by about 100-200 bytes, while RAM usage remains basically unchanged.

### 6.5 Full Configuration (All Features Enabled)

- **RAM Usage**: About 1.4 KB (excluding task stack)
- **Task Stack Usage**: About 1024 bytes (basic task stack 512 bytes + callback tasks 512 bytes)
- **ROM Usage**: About 7.3 KB (only includes msg_manager related code)

**Note**:
- After enabling ENABLE_CALLBACK_INTERRUPT, ROM usage will increase by about 100-200 bytes, and RAM usage remains basically unchanged
- After enabling ENABLE_DYNAMIC_TASK_POOL, task stack usage will be dynamically adjusted based on load, range: 1024-1536 bytes
- Task pool size starts from 2 tasks by default and automatically adjusts based on load, minimum 2 tasks

## 7. Performance Optimization

1. **Use Static Memory Allocation**:
   - Prefer to use `msg_queue_create_static` to create queues
   - Avoid frequent dynamic memory allocation and deallocation

2. **Set Task Priorities Properly**:
   - Set appropriate task priorities according to the importance of message processing
   - Avoid priority inversion

3. **Optimize Message Size**:
   - Minimize message size as much as possible
   - For large data, consider using pointers instead of copying data

4. **Set Timeout Values Reasonably**:
   - Set appropriate timeout values based on the actual execution time of callback functions
   - Avoid too short timeout values that may cause false triggers

## 8. Troubleshooting

### 8.1 Message Sending Failure

- Check if the target queue is registered
- Check if the message queue is full
- Check if memory is sufficient

### 8.2 Callback Function Not Executed

- Check if the message is sent correctly
- Check if the message dispatcher task is running normally
- Check if the callback function is registered correctly

### 8.3 Timeout Mechanism Not Working

- Check if `ENABLE_CALLBACK_TIMEOUT` is defined
- Check if the timeout value is set reasonably
- Check if the task pool has enough tasks

### 8.4 Dynamic Task Pool Not Working

- Check if `ENABLE_DYNAMIC_TASK_POOL` is defined
- Check if task pool size configuration is reasonable
- Check if load calculation is correct
- Check if cooldown period setting is too long

## 9. Example Code

Complete example code can be found in the `msg_demo.c` file, which includes the complete process of message creation, sending, and processing.



## 11. License

This project is licensed under the MIT License, see the LICENSE file for details.