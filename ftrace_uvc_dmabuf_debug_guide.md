# ftrace 调试 UVC 和 DMA-BUF 同步小抄

这份笔记记录一次实际排查路线：

```text
USB 摄像头 -> UVC 驱动 -> V4L2 buffer -> RGA -> DRM 显示
```

现象是 RGA 读摄像头帧前，如果不做 `DMA_BUF_IOCTL_SYNC`，画面可能出现细横条/花屏；加同步后恢复正常。

## 1. ftrace 是什么

`ftrace` 是 Linux 内核自带的函数跟踪工具。它适合回答这种问题：

```text
某个内核函数到底有没有被调用？
是谁调用的？
大概在什么线程/CPU 上运行？
```

它不需要安装 `perf`，只要内核打开了 ftrace 相关配置即可。

## 2. 进入 tracing 目录

不同系统路径可能不同，先这样挂载并进入：

```bash
mount -t tracefs nodev /sys/kernel/tracing 2>/dev/null || \
mount -t debugfs nodev /sys/kernel/debug

cd /sys/kernel/tracing 2>/dev/null || cd /sys/kernel/debug/tracing
```

常用文件：

```text
current_tracer              选择 tracer，比如 nop/function/function_graph
tracing_on                  0 停止，1 开始
trace                       当前 trace 内容
set_ftrace_filter           function 模式下过滤函数
set_graph_function          function_graph 模式下过滤函数
available_filter_functions  当前内核可跟踪函数列表
```

## 3. 查询函数是否可跟踪

先确认当前运行的内核里有没有目标函数：

```bash
grep uvc_video_copy_data_work /proc/kallsyms
grep uvc_video_copy_data_work available_filter_functions
```

模糊查 UVC 相关函数：

```bash
grep -E 'uvc.*copy|uvc.*decode.*data|uvc_video' /proc/kallsyms | head -50
grep -E 'uvc.*copy|uvc.*decode.*data|uvc_video' available_filter_functions | head -50
```

注意：板子正在运行的内核版本可能和当前打开的源码版本不同。比如板子跑 `4.19.232`，但本地看的是 `5.10`，函数名和实现可能有差异。

## 4. 跟踪 UVC 是否发生 copy

先清理旧状态：

```bash
echo 0 > tracing_on
echo nop > current_tracer
echo > trace
echo > set_ftrace_filter
```

只跟踪 UVC copy work：

```bash
echo function > current_tracer
echo uvc_video_copy_data_work > set_ftrace_filter
echo 1 > tracing_on
```

然后另一个终端运行摄像头程序，跑几秒后回来停止：

```bash
echo 0 > tracing_on
cat trace | tail -50
```

如果看到类似输出：

```text
kworker/u9:1-100 [001] .... 162.951284: uvc_video_copy_data_work <-process_one_work
```

说明 UVC 驱动确实通过 workqueue 走到了 `uvc_video_copy_data_work()`。

这行可以这样读：

```text
kworker/u9:1-100       执行线程，是内核 workqueue
[001]                  在 CPU1 上执行
162.951284             时间戳，系统启动后的秒数
uvc_video_copy_data_work 被调用的函数
<-process_one_work     调用者
```

## 5. 同时跟踪多个 UVC 函数

```bash
echo 0 > tracing_on
echo nop > current_tracer
echo > trace
echo > set_ftrace_filter

echo function > current_tracer
echo uvc_video_decode_data >> set_ftrace_filter
echo uvc_video_copy_data_work >> set_ftrace_filter
echo 1 > tracing_on
```

跑摄像头程序后：

```bash
echo 0 > tracing_on
cat trace | tail -100
```

这可以帮助确认：

```text
UVC 收到 USB payload -> decode data -> copy work
```

## 6. 使用 function_graph 看调用耗时

`function_graph` 可以看函数调用树和耗时，开销比 `function` 大，不要长时间开启。

```bash
echo 0 > tracing_on
echo nop > current_tracer
echo > trace
echo > set_graph_function

echo function_graph > current_tracer
echo uvc_video_copy_data_work > set_graph_function
echo 1 > tracing_on
```

跑摄像头程序几秒后：

```bash
echo 0 > tracing_on
cat trace | head -100
```

用完恢复：

```bash
echo 0 > tracing_on
echo nop > current_tracer
echo > set_graph_function
echo > trace
```

## 7. 跟踪 DMA-BUF ioctl 同步路径

用户态类似代码：

```c
struct dma_buf_sync sync = {
    .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ,
};
ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
```

内核大致链路：

```text
dma_buf_ioctl()
  -> dma_buf_begin_cpu_access()
  -> cma_heap_dma_buf_begin_cpu_access()
  -> dma_sync_sgtable_for_cpu()
```

可以用 ftrace 验证：

```bash
echo 0 > tracing_on
echo nop > current_tracer
echo > trace
echo > set_ftrace_filter

echo function > current_tracer
echo dma_buf_ioctl >> set_ftrace_filter
echo dma_buf_begin_cpu_access >> set_ftrace_filter
echo cma_heap_dma_buf_begin_cpu_access >> set_ftrace_filter
echo dma_sync_sgtable_for_cpu >> set_ftrace_filter
echo 1 > tracing_on
```

运行应用后停止查看：

```bash
echo 0 > tracing_on
cat trace
```

如果函数名不存在，先用下面命令找当前内核里的真实名字：

```bash
grep -E 'dma_buf_ioctl|dma_buf_begin_cpu_access|cma.*begin.*cpu|dma_sync.*cpu' available_filter_functions
```

## 8. 为什么 UVC copy 和花屏有关

UVC 摄像头常见路径不是“摄像头硬件直接 DMA 到最终图像 buffer”，而是：

```text
USB host 收到 URB 数据
UVC 驱动解析 payload
内核 kworker 执行 uvc_video_copy_data_work()
memcpy 到 V4L2 video buffer
用户态 DQBUF 拿到帧
RGA 读取同一个 buffer 做颜色转换
```

因为中间有 CPU 写 buffer，数据可能先停留在 CPU cache。RGA 是硬件模块，读的是设备视角下的内存。如果 cache/内存可见性没有处理好，RGA 可能读到部分旧数据，于是画面出现横条或局部花屏。

`DMA_BUF_IOCTL_SYNC` 对 dma-buf exporter 触发 cache 同步，在 CMA heap 上会进入类似：

```text
cma_heap_dma_buf_begin_cpu_access()
  -> dma_sync_sgtable_for_cpu()
```

所以它虽然名字像 CPU access，同步效果却能影响后续设备读取同一块共享 buffer 时看到的数据。

## 9. 收尾清理命令

调完以后建议恢复 ftrace 状态：

```bash
echo 0 > tracing_on
echo nop > current_tracer
echo > set_ftrace_filter
echo > set_graph_function
echo > trace
```

## 10. 排查口诀

```text
源码里猜半天，不如 trace 一下。

先确认函数存在：
  /proc/kallsyms
  available_filter_functions

再确认函数被调用：
  function tracer

需要看调用树/耗时：
  function_graph tracer

需要看热点占比：
  perf
```
