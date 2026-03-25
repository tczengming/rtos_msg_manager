# 消息管理器 (msg_manager) 使用指南

## 1. 概述

消息管理器是一个基于 FreeRTOS 的轻量级消息传递系统，提供了线程安全的消息发送和接收机制，支持回调超时检测和中断处理。做了移植预留，方便在其他RTOS上使用。

## 2. 功能特性

- 线程安全的消息发送和接收
- 支持回调函数处理消息
- 可选的回调超时检测机制
- 可选的回调中断处理机制
- 消息池管理，优化内存使用
- 支持静态内存分配，无需动态内存

## 3. 关键宏定义

### 3.1 ENABLE_CALLBACK_TIMEOUT

**功能**：启用回调超时检测机制

**作用**：
- 当回调函数执行时间超过设定的超时时间时，会检测到超时并发出提醒
- 防止单个回调函数执行时间过长，影响整个系统的响应性能
- 提高系统的可靠性和稳定性

**RAM 占用增加**：
- **任务池**：每个任务占用约 256 字节栈空间，默认 2 个任务
- 定时器：每个任务一个定时器，占用约 64 字节
- 总增加：约 640 字节

**ROM 占用增加**：
- 任务池管理代码：约 1.5 KB
- 超时检测代码：约 0.5 KB
- 总增加：约 2 KB

### 3.2 ENABLE_CALLBACK_INTERRUPT

- **功能**：启用真正中断阻塞回调的功能，当回调函数执行超时时，会通过中断方式强制终止回调函数的执行。
- **默认值**：未定义（禁用）
- **使用场景**：当回调函数可能出现无限循环或严重阻塞时，需要强制终止回调函数的执行。
- **依赖**：需要与 ENABLE_CALLBACK_TIMEOUT 一起使用，单独使用无效。

### 3.3 ENABLE_DYNAMIC_TASK_POOL

**功能**：启用动态任务池大小调整功能

**作用**：
- 根据系统负载动态调整任务池大小，优化系统资源使用
- 当负载过高时，自动增加任务池大小
- 当负载过低时，自动减少任务池大小
- 避免任务池过大导致的资源浪费，或过小导致的任务等待

**配置参数**：
- `MIN_TASK_POOL_SIZE`：最小任务池大小，默认 2（必须大于等于2，否则超时中止功能会失效）
- `MAX_TASK_POOL_SIZE`：最大任务池大小，默认 4
- `DEFAULT_TASK_POOL_SIZE`：默认任务池大小，默认 2
- `TASK_POOL_ADJUST_INTERVAL_MS`：任务池调整间隔，默认 10000ms
- `TASK_POOL_COOLDOWN_PERIOD_MS`：任务池调整冷却期，默认 60000ms
- `TASK_POOL_LOAD_THRESHOLD`：任务池负载阈值，默认 80%

**重要说明**：
- 任务池最小大小必须设置为 2 或更大，否则超时中止功能会失效
- 这是因为当一个任务执行回调时，需要另一个任务来处理超时检测和中止操作

**RAM 占用影响**：
- 任务池大小会根据负载动态调整，范围：2-4 个任务
- 每个任务占用约 256 字节栈空间
- 每个任务一个定时器，占用约 64 字节
- 动态调整时，RAM 占用会根据当前任务池大小变化

**ROM 占用增加**：
- 动态任务池管理代码：约 1 KB
- 负载计算和调整逻辑：约 0.5 KB
- 总增加：约 1.5 KB

### 3.4 消息级回调

- **功能**：支持在消息中设置回调函数，消息被处理时会优先执行消息级回调，而不是队列级回调。
- **使用场景**：当不同消息需要不同处理逻辑时，使用消息级回调可以避免在队列回调中进行大量的类型判断。



## 4. 使用方法

### 4.1 初始化消息管理器

```c
#include "msg_manager.h"

void app_init(void) {
    // 初始化消息管理器
    msg_manager_init();
}
```

### 4.2 定义消息结构

```c
// 定义消息结构
typedef struct my_data_msg {
    msg_base base; // 必须是第一个成员，方便强转
    int sensor_id;
    float value;
} my_data_msg;

// 实现 destroy 函数
void my_data_msg_destroy(msg_base* self) {
    my_data_msg* p = (my_data_msg*)self;
    // 如果消息里面有额外的指针动态分配了内存，要在这里 free
    // 否则这里不要free,消息池会自动处理内存管理
}

// 工厂函数 (替代 new/make_unique)
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

### 4.3 注册消息处理回调

```c
// 回调函数
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
    
    // 释放消息
    msg_manager_free_msg(msg);
}

// 注册消息队列
msg_handle* normal_handle = msg_manager_register(my_process_callback, -1);
```

### 4.4 发送消息

#### 通过句柄发送消息

```c
// 创建消息
my_data_msg* msg = my_data_msg_create(1, 10.0);

// 发送消息
msg_queue_code result = msg_manager_send_msg_to(normal_handle, (msg_base*)msg);
if (result != MSG_QUEUE_CODE_OK) {
    os_log("Failed to send message, error code: %d", result);
    msg_manager_free_msg((msg_base*)msg);
}
```

#### 通过队列ID发送消息

```c
// 获取队列ID
uint8_t queue_id = normal_handle->id;

// 创建消息
my_data_msg* msg = my_data_msg_create(1, 10.0);

// 通过ID发送消息
msg_queue_code result = msg_manager_send_msg_to_id(queue_id, (msg_base*)msg);
if (result != MSG_QUEUE_CODE_OK) {
    os_log("Failed to send message, error code: %d", result);
    msg_manager_free_msg((msg_base*)msg);
}
```

#### 使用enum来标识队列ID

```c
// 1. 定义队列ID枚举（在msg_demo.c中定义）
typedef enum {
    MSG_QUEUE_ID_NORMAL = 1,    // 正常消息队列ID
    MSG_QUEUE_ID_BLOCKING,       // 阻塞消息队列ID
    MSG_QUEUE_ID_MAX             // 最大队列ID，用于边界检查
} msg_queue_id_t;

// 2. 使用指定的队列ID注册队列
msg_handle* normal_handle = msg_manager_register_with_id(MSG_QUEUE_ID_NORMAL, my_process_callback, -1);
msg_handle* blocking_handle = msg_manager_register_with_id(MSG_QUEUE_ID_BLOCKING, my_blocking_callback, -1);

// 3. 在其他任务中直接使用enum值发送消息
my_data_msg* msg = my_data_msg_create(1, 10.0);
msg_queue_code result = msg_manager_send_msg_to_id(MSG_QUEUE_ID_NORMAL, (msg_base*)msg);
if (result != MSG_QUEUE_CODE_OK) {
    os_log("Failed to send message, error code: %d", result);
    msg_manager_free_msg((msg_base*)msg);
}
```

#### 使用消息级回调

```c
// 1. 定义消息级回调函数
void my_special_callback(msg_base* msg) {
    if (!msg) return;
    
    my_data_msg* d_msg = (my_data_msg*)msg;
    os_log("Special handling: Sensor %d, Value %.2f", d_msg->sensor_id, d_msg->value);
    
    // 不需要手动释放消息，分发器会自动释放
}

// 2. 创建带回调的消息
my_data_msg* msg = my_data_msg_create_with_callback(1, 10.0, my_special_callback);

// 3. 发送消息
msg_queue_code result = msg_manager_send_msg_to_id(MSG_QUEUE_ID_NORMAL, (msg_base*)msg);
if (result != MSG_QUEUE_CODE_OK) {
    os_log("Failed to send message, error code: %d", result);
    msg_manager_free_msg((msg_base*)msg);
}
```

### 4.5 自动释放机制

消息管理器实现了自动释放机制，具体特点如下：

1. **自动释放**：消息分发器在处理完消息后会自动调用 `msg_manager_free_msg` 释放消息，回调函数不需要手动释放消息

2. **回调优先级**：
   - 优先使用消息级回调（`msg->callback`）
   - 如果消息级回调为 NULL，则使用队列级回调（注册队列时指定的回调）

3. **内存管理**：
   - 消息池会自动管理消息的内存分配和释放
   - 对于超过消息池大小的消息，会使用动态内存分配

4. **使用建议**：
   - 回调函数中不要手动调用 `msg_manager_free_msg`，否则会导致重复释放
   - 对于需要特殊处理的消息，使用消息级回调
   - 对于需要统一处理的消息类型，使用队列级回调

5. **向后兼容**：
   - 现有的使用队列级回调的代码无需修改
   - 新代码可以选择使用消息级回调或队列级回调


### 4.5 注销消息队列

```c
// 通过句柄注销
msg_manager_unregister_by_handle(normal_handle);

// 通过ID注销
msg_manager_unregister_by_id(handle->id);
```

## 5. 移植指南

### 5.1 依赖项

- FreeRTOS 或其他支持的 RTOS
- C 标准库
- OS 适配器层

### 5.2 移植步骤

1. **修改 OS 适配器**：
   - 在 `os_adapter.h` 和 `os_adapter.c` 中实现目标系统的接口
   - 确保所有必要的函数都有实现

2. **配置内存管理**：
   - 根据目标系统的内存管理方式，修改 `os_malloc` 和 `os_free` 函数

3. **配置任务管理**：
   - 根据目标系统的任务管理方式，修改任务创建、删除、延迟等函数

4. **配置队列管理**：
   - 根据目标系统的队列管理方式，修改队列创建、发送、接收等函数

5. **配置定时器**：
   - 根据目标系统的定时器管理方式，修改定时器创建、启动、停止等函数

6. **配置互斥锁**：
   - 根据目标系统的互斥锁管理方式，修改互斥锁创建、获取、释放等函数

7. **配置日志**：
   - 根据目标系统的日志系统，修改 `os_log` 函数

### 5.3 移植示例

#### 移植到 Linux 系统

```c
// 修改 os_adapter.h
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>

// 类型定义
typedef pthread_t os_task_handle;
typedef sem_t os_semaphore_handle;
typedef timer_t os_timer_handle;
typedef void* os_queue_handle;
typedef uint32_t os_tick_t;
typedef void (*os_task_func)(void *);
typedef void (*os_timer_func)(void *);

// 实现 os_adapter.c
void* os_malloc(size_t size) {
    return malloc(size);
}

void os_free(void* ptr) {
    free(ptr);
}

// 其他函数实现...
```

## 6. RAM 和 ROM 占用分析

### 6.1 基础功能（默认配置）

- **RAM 占用**：约 1 KB
  - 消息管理器结构：约 256 字节
  - 消息池：约 512 字节（MSG_POOL_SIZE=10，每个消息项 64 字节）
  - 队列缓冲区：约 256 字节（MSG_QUEUE_MAX_ITEMS=20，每个消息指针 4 字节）
  - **任务栈**：不包含在内（由用户配置）

- **ROM 占用**：约 3.7 KB
  - msg_manager.c：约 2.7 KB（包含新增的 msg_manager_send_msg_to_id 和 msg_manager_register_with_id 函数）
  - msg_queue.c：约 1 KB
  - 不包含 main.c 等其他文件的代码

### 6.2 基础任务栈大小

- **基础任务栈大小**：
  - 消息分发器任务（包含回调处理）：128 字（512 字节）

- **栈大小配置**：
  - 消息分发器任务栈大小由 `MSG_DISPATCHER_STACK_SIZE` 宏定义
  - 回调任务栈大小由 `configMINIMAL_STACK_SIZE * 2` 定义（在启用 ENABLE_CALLBACK_TIMEOUT 时使用）

### 6.3 启用 ENABLE_CALLBACK_TIMEOUT

- **RAM 占用增加**：约 1280 字节（固定任务池）
  - 任务池数据结构：约 256 字节
  - 定时器：4 个定时器 × 64 字节 = 256 字节
  - **任务池**：2 个回调任务 × 256 字节 = 512 字节（单独计算）

- **ROM 占用增加**：约 1.5 KB
  - 任务池管理代码：约 1 KB
  - 超时检测代码：约 0.5 KB

### 6.4 启用 ENABLE_DYNAMIC_TASK_POOL

- **RAM 占用影响**：
  - 任务池大小会根据负载动态调整，范围：2-4 个任务
  - 每个任务占用约 256 字节栈空间
  - 每个任务一个定时器，占用约 64 字节
  - RAM 占用会根据当前任务池大小变化

- **ROM 占用增加**：约 1.5 KB
  - 动态任务池管理代码：约 1 KB
  - 负载计算和调整逻辑：约 0.5 KB

### 6.4 启用 ENABLE_CALLBACK_INTERRUPT

如果需要使用真正的中断阻塞回调功能，可以在 `msg_manager.h` 中取消注释 `ENABLE_CALLBACK_INTERRUPT` 宏定义：

```c
// 定义此宏启用真正中断阻塞回调的功能
#define ENABLE_CALLBACK_INTERRUPT
```

这样，当回调函数执行超时时，系统会通过中断方式强制终止回调函数的执行，确保消息处理不会被长时间阻塞。

**注意**：ENABLE_CALLBACK_INTERRUPT 仅添加了少量代码，对 RAM 和 ROM 占用影响很小，几乎可以忽略不计。

### 6.5 完整配置（启用所有功能）

- **RAM 占用**：约 1.4 KB（不包含任务栈）
- **任务栈占用**：约 1024-1536 字节（基础任务栈 512 字节 + 回调任务 512-1024 字节，动态调整）
- **ROM 占用**：约 7.3 KB（仅包含 msg_manager 相关代码）

**说明**：
- 启用 ENABLE_CALLBACK_INTERRUPT 后，ROM 占用会增加约 100-200 字节，RAM 占用基本不变
- 启用 ENABLE_DYNAMIC_TASK_POOL 后，任务栈占用会根据负载动态调整，范围：1024-1536 字节
- 任务池大小默认从 2 个任务开始，根据负载自动调整，最小为 2 个任务

## 7. 性能优化

1. **使用静态内存分配**：
   - 优先使用 `msg_queue_create_static` 创建队列
   - 避免频繁的动态内存分配和释放

2. **合理设置任务优先级**：
   - 根据消息处理的重要性设置合适的任务优先级
   - 避免优先级反转

3. **优化消息大小**：
   - 尽量减少消息的大小
   - 对于大数据，考虑使用指针而非复制数据

4. **合理设置超时时间**：
   - 根据实际回调函数的执行时间设置合适的超时时间
   - 避免过短的超时时间导致误触发

## 8. 故障排查

### 8.1 消息发送失败

- 检查目标队列是否已注册
- 检查消息队列是否已满
- 检查内存是否足够

### 8.2 回调函数未执行

- 检查消息是否正确发送
- 检查消息分发器任务是否正常运行
- 检查回调函数是否正确注册

### 8.3 超时机制不工作

- 检查 `ENABLE_CALLBACK_TIMEOUT` 是否已定义
- 检查超时时间是否设置合理
- 检查任务池是否有足够的任务

### 8.4 动态任务池不工作

- 检查 `ENABLE_DYNAMIC_TASK_POOL` 是否已定义
- 检查任务池大小配置是否合理
- 检查负载计算是否正确
- 检查冷却期设置是否过长

## 9. 示例代码

完整的示例代码可参考 `msg_demo.c` 文件，其中包含了消息创建、发送、处理的完整流程。



## 11. 许可证

本项目采用 MIT 许可证，详见 LICENSE 文件。
# Running with VSCode Launch Configurations

## Prerequisites
* Install [C/C++ extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools) in VSCode.
* Install [arm-none-eabi-gcc](https://developer.arm.com/tools-and-software/open-source-software/developer-tools/gnu-toolchain/gnu-rm/downloads).
* Install GNU make utility.
* Ensure the required binaries are in PATH with ```arm-none-eabi-gcc --version```, ```arm-none-eabi-gdb --version```, and ```make --version```.

## Building and Running
1. Open VSCode to the folder ```FreeRTOS/Demo/CORTEX_MPS2_QEMU_IAR_GCC```.
2. Open ```.vscode/launch.json```, and ensure the ```miDebuggerPath``` variable is set to the path where arm-none-eabi-gdb is on your machine.
3. Open ```main.c```, and set ```mainCREATE_SIMPLE_BLINKY_DEMO_ONLY``` to ```1``` to generate just the [simply blinky demo](https://www.freertos.org/a00102.html#simple_blinky_demo).
4. On the VSCode left side panel, select the “Run and Debug” button. Then select “Launch QEMU RTOSDemo” from the dropdown on the top right and press the play button. This will build, run, and attach a debugger to the demo program.

## Tracing with Percepio View
This demo project includes Percepio TraceRecorder, configured for snapshot tracing with Percepio View or Tracealyzer.
Percepio View is a free tracing tool from Percepio, providing the core features of Percepio Tracealyzer but limited to snapshot tracing.
No license or registration is required. More information and download is found at [Percepio's product page for Percepio View](https://traceviewer.io/get-view?target=freertos).

### TraceRecorder Integration
If you like to study how TraceRecorder is integrated, the steps for adding TraceRecorder are tagged with "TODO TraceRecorder" comments in the demo source code.
This way, if using an Eclipse-based IDE, you can find a summary in the Tasks window by selecting Window -> Show View -> Tasks (or Other, if not listed).
See also [the official getting-started guide](https://traceviewer.io/getting-started-freertos-view).

### Usage with GDB
To save the TraceRecorder trace, start a debug session with GDB.
Halt the execution and the run the command below. 
This saves the trace as trace.bin in the build/gcc folder.
Open the trace file in Percepio View or Tracealyzer.

If using an Eclipse-based IDE, run the following command in the "Debugger Console":
```
dump binary value trace.bin *RecorderDataPtr
```

If using VS Code, use the "Debug Console" and add "-exec" before the command:
```
-exec dump binary value trace.bin *RecorderDataPtr
```

Note that you can typically copy/paste this command into the debug console.


### Usage with IAR Embedded Workbench for Arm
Launch the IAR debugger. With the default project configuration, this should connect to the QEMU GDB server.
To save the trace, please refer to the "Snapshot Mode" guide at [https://percepio.com/iar](https://percepio.com/iar).
In summary:
- Download the IAR macro file [save_trace_buffer.mac](https://percepio.com/downloads/save_trace_buffer.mac) (click "save as")
- Add the macro file in the project options -> Debugger -> Use Macro File(s). 
- Start debugging and open View -> Macros -> Debugger Macros.
- Locate and run "save_trace_buffer". Open the resulting "trace.hex" in Percepio View or Tracealyzer.