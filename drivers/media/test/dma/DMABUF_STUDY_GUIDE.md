# dmabuf 学习指南

这份指南按你的当前路线写：已经理解 DMA buffer、cache sync、字符设备 mmap，下一步面向嵌入式 Linux 音视频零拷贝。

## 先给结论

你现在可以开始学 dmabuf，但建议先学“怎么用 dmabuf 串音视频链路”，不要一上来就手写完整 exporter。

推荐路线：

```text
字符设备 mmap / DMA cache
  ↓
V4L2_MEMORY_MMAP
  ↓
VIDIOC_EXPBUF
  ↓
V4L2_MEMORY_DMABUF
  ↓
RGA / MPP / DRM import dmabuf fd
  ↓
回头看 dma_buf_ops / exporter / importer 内核实现
```

dmabuf 的核心不是“让 DMA 能搬数据”，而是：

```text
让同一块 buffer 在多个驱动、多个硬件、用户态进程之间共享。
```

## 为什么你需要学 dmabuf

音视频链路里经常是多个模块接力处理同一帧：

```text
Camera / V4L2
  ↓
RGA 缩放/旋转/格式转换
  ↓
MPP/VPU 编码
  ↓
DRM/KMS 显示
```

如果每一环都 copy 一次，会变成：

```text
camera buffer
  -> CPU copy
RGA buffer
  -> CPU copy
encoder buffer
  -> CPU copy
display buffer
```

这不叫零拷贝。

dmabuf 的目标是：

```text
camera 产生一块 buffer
  ↓
导出 dmabuf fd
  ↓
RGA import 这个 fd
  ↓
MPP import 这个 fd
  ↓
DRM import 这个 fd
```

用户态传的是 fd，不是图像数据。

## dmabuf 和 mmap 的关系

你现在已经理解字符设备 mmap：

```text
驱动申请 buffer
  ↓
用户态 mmap
  ↓
用户态虚拟地址和内核 buffer 指向同一块底层内存
```

dmabuf 更进一步：

```text
一个驱动导出 buffer fd
  ↓
另一个驱动通过 fd 导入这块 buffer
  ↓
多个硬件可以围绕同一块 buffer 工作
```

mmap 解决的是：

```text
用户态怎么访问这块 buffer
```

dmabuf 解决的是：

```text
这块 buffer 怎么跨驱动、跨子系统共享
```

dmabuf 本身也支持 mmap：

```c
dma_buf_mmap()
```

所以 mmap 不是被 dmabuf 替代，而是变成 dmabuf 的一个能力。

## 基本概念

### dmabuf fd

dmabuf fd 是用户态拿到的一个文件描述符。

它代表一块可以被共享的 DMA buffer。

用户态不会直接看到物理地址或 DMA 地址，而是传递这个 fd：

```text
fd = 一块 buffer 的句柄
```

### exporter

exporter 是 buffer 的拥有者和导出者。

它负责：

```text
申请 buffer
管理 buffer 生命周期
实现 dma_buf_ops
导出 dmabuf fd
处理 CPU 访问同步
处理 importer attach/map
```

例子：

```text
V4L2 摄像头驱动导出一帧 buffer
ION/CMA/heap 导出一块通用 buffer
DRM dumb buffer 导出显示 buffer
```

### importer

importer 是 buffer 的使用者。

它不拥有 buffer，但想让自己的硬件访问这块 buffer。

它通常会做：

```text
通过 fd 得到 dma_buf
attach 到自己的 struct device
map_attachment 得到 sg_table
把 sg_table 映射成自己设备可用的 DMA 地址
硬件开始处理
unmap/detach
```

例子：

```text
RGA import 摄像头帧 fd 做缩放
MPP import 图像 fd 做编码
DRM import 图像 fd 做显示
```

### attach

attach 表示：

```text
某个设备准备使用这块 dmabuf
```

它会把 importer 的 `struct device *` 和 dmabuf 绑定起来。

为什么需要 device？

因为不同硬件设备可能有不同的：

```text
DMA mask
IOMMU domain
cache coherency
地址访问能力
```

### map_attachment

map_attachment 表示：

```text
把这块 dmabuf 映射成当前 importer 设备可用的 DMA 视图。
```

通常会得到：

```c
struct sg_table *sgt;
```

真实硬件最后通常不是拿 fd 工作，而是拿 sg table / DMA address 工作。

### begin_cpu_access / end_cpu_access

当 CPU 要读写 dmabuf 时，需要明确同步边界：

```text
begin_cpu_access
  ↓
CPU 读写 buffer
  ↓
end_cpu_access
```

这和你之前学的 cache sync 是同一类问题。

粗略理解：

```text
设备写完，CPU 要读：
    begin_cpu_access 让 CPU 看到设备写的新数据

CPU 写完，设备要读：
    end_cpu_access 让设备看到 CPU 写的新数据
```

## 和 DMA API 的关系

不要把 dmabuf 和 `dma_map_single()` 看成替代关系。

它们不是一层东西。

```text
dma_map_single / dma_map_sg
    解决某个设备如何 DMA 访问某块内存。

dmabuf
    解决某块 buffer 如何在多个驱动/设备之间共享。
```

在 importer 真正使用 dmabuf 时，底层仍然可能走：

```c
dma_map_sg()
dma_unmap_sg()
```

你之前学的 cache sync、DMA 地址、sg table 仍然有用。

## 第一阶段：先学 V4L2 MMAP

先把 V4L2 普通 mmap buffer 流程跑通。

用户态典型流程：

```text
open /dev/videoX
  ↓
VIDIOC_QUERYCAP
  ↓
VIDIOC_S_FMT
  ↓
VIDIOC_REQBUFS, memory = V4L2_MEMORY_MMAP
  ↓
VIDIOC_QUERYBUF
  ↓
mmap
  ↓
VIDIOC_QBUF
  ↓
VIDIOC_STREAMON
  ↓
VIDIOC_DQBUF
  ↓
读一帧
  ↓
VIDIOC_QBUF
  ↓
循环
```

你要重点理解：

```text
REQBUFS 是申请 buffer
QUERYBUF 是查询每个 buffer 的 offset/size
mmap 是把 buffer 映射到用户态
QBUF 是把空 buffer 交给驱动/硬件
DQBUF 是硬件填好一帧后还给用户态
```

这一步对应你现在的字符设备 mmap 模板。

## 第二阶段：学 VIDIOC_EXPBUF

`VIDIOC_EXPBUF` 是 V4L2 把某个 buffer 导出成 dmabuf fd。

流程大概是：

```text
V4L2 REQBUFS 申请 buffer
  ↓
VIDIOC_EXPBUF 导出第 n 个 buffer
  ↓
得到 dmabuf fd
  ↓
把 fd 交给别的模块
```

你要理解：

```text
同一块 V4L2 buffer
既可以 mmap 给 CPU 看
也可以 export 成 dmabuf fd 给别的设备用
```

需要重点看：

```c
struct v4l2_exportbuffer
VIDIOC_EXPBUF
```

关键字段：

```text
type   buffer 类型，例如 V4L2_BUF_TYPE_VIDEO_CAPTURE
index  第几个 V4L2 buffer
flags  fd flags，例如 O_CLOEXEC
fd     ioctl 成功后返回的 dmabuf fd
```

## 第三阶段：学 V4L2_MEMORY_DMABUF

`V4L2_MEMORY_DMABUF` 是另一种方向：

```text
不是 V4L2 自己申请 buffer，
而是用户态拿一批 dmabuf fd 交给 V4L2 使用。
```

典型流程：

```text
用户态从其他模块拿到 dmabuf fd
  ↓
VIDIOC_QBUF 时把 fd 填给 V4L2
  ↓
V4L2 驱动 import 这块 buffer
  ↓
摄像头/编码器/硬件使用这块 buffer
```

你要重点看：

```c
struct v4l2_buffer
buf.memory = V4L2_MEMORY_DMABUF
buf.m.fd = dmabuf_fd
```

这一步很重要，因为真实零拷贝经常是多个子系统互相传 fd。

## 第四阶段：结合 RK 平台

在 RK3568 音视频链路里，你后面大概率会遇到：

```text
V4L2
RGA
MPP
DRM/KMS
```

你要关注它们是否支持 dmabuf import/export。

建议实验方向：

```text
V4L2 采集一帧
  ↓
VIDIOC_EXPBUF 得到 fd
  ↓
RGA import fd 做格式转换或缩放
  ↓
MPP import fd 编码
```

先不用急着自己写 exporter。

先学会：

```text
谁产出 fd
谁消费 fd
fd 生命周期谁管理
什么时候 close(fd)
CPU 是否访问过 buffer
是否需要 begin/end CPU access
```

## 第五阶段：再看内核实现

等你能用 dmabuf fd 串起两个模块，再回头看内核实现。

需要学的核心结构：

```c
struct dma_buf
struct dma_buf_ops
struct dma_buf_attachment
struct sg_table
```

核心 API：

```c
dma_buf_export()
dma_buf_fd()
dma_buf_get()
dma_buf_put()
dma_buf_attach()
dma_buf_detach()
dma_buf_map_attachment()
dma_buf_unmap_attachment()
dma_buf_begin_cpu_access()
dma_buf_end_cpu_access()
```

先理解每个 API 在生命周期里的位置，不要一上来背参数。

## exporter 生命周期

如果以后你要自己写 exporter，大概是：

```text
申请 buffer
  ↓
填 dma_buf_export_info
  ↓
dma_buf_export
  ↓
dma_buf_fd 得到 fd
  ↓
用户态拿 fd 传给别人
  ↓
最后 release 回调释放 buffer
```

`dma_buf_ops` 里常见回调：

```text
attach
detach
map_dma_buf
unmap_dma_buf
begin_cpu_access
end_cpu_access
mmap
vmap
vunmap
release
```

这里的难点不是 API，而是所有权和同步：

```text
buffer 谁拥有？
fd 什么时候关闭？
exporter 什么时候能释放？
CPU 和设备谁正在访问？
cache 什么时候同步？
```

## importer 生命周期

如果以后你要在驱动里 import dmabuf，大概是：

```text
用户态传入 dmabuf fd
  ↓
dma_buf_get(fd)
  ↓
dma_buf_attach(dmabuf, dev)
  ↓
dma_buf_map_attachment(attach, direction)
  ↓
拿 sg_table 给硬件使用
  ↓
硬件完成
  ↓
dma_buf_unmap_attachment()
  ↓
dma_buf_detach()
  ↓
dma_buf_put()
```

这个流程和你学过的 DMA map/unmap 很像，只是对象从普通内存变成了共享 buffer。

## CPU 访问 dmabuf

如果 CPU 要读写 dmabuf，要关注：

```text
dma_buf_begin_cpu_access()
dma_buf_end_cpu_access()
```

比如 CPU 要读设备写完的数据：

```text
设备完成写 buffer
  ↓
dma_buf_begin_cpu_access(..., DMA_FROM_DEVICE)
  ↓
CPU 读 buffer
  ↓
dma_buf_end_cpu_access(..., DMA_FROM_DEVICE)
```

CPU 写完后设备要读：

```text
dma_buf_begin_cpu_access(..., DMA_TO_DEVICE)
  ↓
CPU 写 buffer
  ↓
dma_buf_end_cpu_access(..., DMA_TO_DEVICE)
  ↓
设备读 buffer
```

不同 exporter 的实现细节可能不同，但这个 ownership 思路不变。

## 你现在不用急着学的内容

可以先放后：

```text
完整手写 dma_buf_ops exporter
复杂 reservation object / fence
同步文件 sync_file
跨进程 fence 机制
DRM PRIME 深层实现
IOMMU domain 细节
```

这些以后做复杂图形/显示/多硬件同步时再看。

你现在优先级最高的是：

```text
V4L2 buffer 模型
VIDIOC_EXPBUF
V4L2_MEMORY_DMABUF
RGA/MPP import dmabuf fd
```

## 学习检查清单

学完第一轮后，你应该能回答这些问题：

```text
dmabuf fd 代表什么？
exporter 和 importer 分别是谁？
V4L2 的 VIDIOC_EXPBUF 在导出什么？
V4L2_MEMORY_DMABUF 和 V4L2_MEMORY_MMAP 有什么区别？
为什么 importer 要 attach 到自己的 struct device？
map_attachment 返回的 sg_table 是给谁用的？
CPU 访问 dmabuf 前后为什么要 begin/end_cpu_access？
close(fd) 是否等于 buffer 立刻释放？
为什么 dmabuf 不等于物理地址？
为什么 dmabuf 不等于 dma_map_single？
```

## 建议实践顺序

### 实验 1：V4L2 MMAP 采集

写一个最小 V4L2 采集程序：

```text
REQBUFS
QUERYBUF
mmap
QBUF
STREAMON
DQBUF
保存一帧
```

目标：

```text
理解 V4L2 buffer 生命周期。
```

### 实验 2：V4L2 EXPBUF

在实验 1 基础上，对每个 V4L2 buffer 调：

```text
VIDIOC_EXPBUF
```

打印每个 buffer 对应的 dmabuf fd。

目标：

```text
理解 V4L2 buffer 可以导出成 fd。
```

### 实验 3：把 fd 交给另一个模块

找一个 RK 平台上支持 dmabuf import 的模块，例如 RGA。

目标：

```text
V4L2 采集 buffer 不经 CPU copy，直接交给 RGA。
```

### 实验 4：再看内核源码

回头看：

```text
drivers/media/common/videobuf2/
drivers/dma-buf/
drivers/gpu/drm/
```

目标：

```text
把用户态 fd 流程和内核 exporter/importer 实现对应起来。
```

## 一句话记忆

```text
mmap 让用户态直接碰 buffer。
dmabuf 让多个驱动/硬件共享同一块 buffer。
V4L2 buffer 模型告诉你视频帧如何排队流动。
```

你现在学 dmabuf 的正确姿势是：

```text
先把 fd 在 V4L2/RGA/MPP 之间跑起来，
再回头理解 dma_buf_ops 怎么实现。
```
