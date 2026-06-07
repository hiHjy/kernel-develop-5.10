# 字符设备 mmap 学习笔记

这份笔记对应 `dma_chardev_template.c` 里的 `dma_template_mmap()` 回调。

## mmap 解决什么问题

普通 `read/write/ioctl` 传大块数据时，通常需要在用户态和内核态之间拷贝。

音视频数据一帧可能很大，如果每次都 copy，性能会很差。`mmap` 的意义是把驱动里的 buffer 映射到用户进程地址空间，让用户态和内核驱动访问同一块底层内存。

映射成功后会同时存在三种地址：

```text
用户态虚拟地址：mmap() 返回值，app 读写用
内核虚拟地址：ctx->src_cpu_addr / ctx->dst_cpu_addr，驱动读写用
DMA 地址：ctx->src_dma_addr / ctx->dst_dma_addr，硬件 DMA 用
```

这三种地址不是同一个概念，不能混用。

## 推荐使用流程

用户态建议按这个顺序走：

```text
open /dev/dma_chardev_template
  ↓
ioctl DMA_TEST_IOC_ALLOC 申请 src/dst buffer
  ↓
ioctl DMA_TEST_IOC_INFO 获取 size 和 mmap offset
  ↓
mmap src buffer
  ↓
mmap dst buffer
  ↓
用户态写 src
  ↓
ioctl DMA_TEST_IOC_START 触发后续 DMA copy
  ↓
用户态检查 dst
  ↓
munmap src/dst
  ↓
ioctl DMA_TEST_IOC_FREE
  ↓
close
```

用户态伪代码：

```c
fd = open("/dev/dma_chardev_template", O_RDWR);

alloc.size = 4096;
ioctl(fd, DMA_TEST_IOC_ALLOC, &alloc);

ioctl(fd, DMA_TEST_IOC_INFO, &info);

src = mmap(NULL, info.size, PROT_READ | PROT_WRITE,
           MAP_SHARED, fd, info.src_mmap_offset);

dst = mmap(NULL, info.size, PROT_READ | PROT_WRITE,
           MAP_SHARED, fd, info.dst_mmap_offset);

memset(src, 0x5a, info.size);

ioctl(fd, DMA_TEST_IOC_START);

/* 后面检查 dst */
```

## mmap 回调参数

内核回调原型：

```c
static int dma_template_mmap(struct file *filp, struct vm_area_struct *vma)
```

`filp` 来自用户态 `open()` 得到的 fd。

模板里在 `open()` 中做了：

```c
filp->private_data = ctx;
```

所以 `mmap()` 里可以拿回设备上下文：

```c
struct dma_chardev_ctx *ctx = filp->private_data;
```

`vma` 描述用户进程里即将建立的一段虚拟地址区域。先重点理解这些字段：

```text
vma->vm_start    用户态虚拟地址起点
vma->vm_end      用户态虚拟地址终点
vma->vm_pgoff    用户态 mmap offset 转成页之后的值
vma->vm_flags    映射权限和属性
```

映射大小：

```c
req_size = vma->vm_end - vma->vm_start;
```

用户态传入的字节 offset：

```c
offset = vma->vm_pgoff << PAGE_SHIFT;
```

注意：`vma->vm_pgoff` 的单位是页，不是字节。

## 用 offset 区分 src 和 dst

一个 fd 只有一个 mmap 回调，但模板里有两块 buffer：

```text
src buffer
dst buffer
```

所以约定：

```text
offset = 0                 映射 src
offset = PAGE_ALIGN(size)  映射 dst
```

内核判断：

```c
if (offset == 0) {
    cpu_addr = ctx->src_cpu_addr;
    dma_addr = ctx->src_dma_addr;
} else if (offset == PAGE_ALIGN(ctx->size)) {
    cpu_addr = ctx->dst_cpu_addr;
    dma_addr = ctx->dst_dma_addr;
} else {
    return -EINVAL;
}
```

为什么 dst offset 要用 `PAGE_ALIGN(size)`？

因为用户态 `mmap()` 的 offset 必须页对齐。

## dma_mmap_coherent 的作用

`dma_alloc_coherent()` 申请 buffer：

```c
cpu_addr = dma_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);
```

得到：

```text
cpu_addr  内核虚拟地址
dma_addr  DMA 地址
```

但是用户态还不能访问。

`dma_mmap_coherent()` 的作用是把这块 coherent DMA buffer 映射到用户态 VMA：

```c
ret = dma_mmap_coherent(ctx->dev, vma, cpu_addr, dma_addr, ctx->size);
```

成功后：

```text
用户态 mmap 返回地址
内核 cpu_addr
硬件 dma_addr
```

三者指向同一块底层 buffer。

## 重要坑：选择 dst 后要清 vma->vm_pgoff

这个坑很容易踩。

模板用 mmap offset 区分 src/dst：

```text
offset 0     -> src
offset 4096  -> dst
```

但是 `dma_mmap_coherent()` 自己也会看 `vma->vm_pgoff`。

如果用户态映射 dst：

```c
mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
```

内核里：

```text
vma->vm_pgoff = 1
```

驱动用它选中 dst buffer 后，如果直接调用：

```c
dma_mmap_coherent(ctx->dev, vma, dst_cpu, dst_dma, 4096);
```

`dma_mmap_coherent()` 会以为你要映射 dst buffer 内部偏移 4096 之后的内容。

但 dst buffer 总共才 4096 字节，于是越界，常见错误是：

```text
mmap failed: No such device or address
errno = 6
ENXIO
```

正确做法是在选中 src/dst 后，把 `vma->vm_pgoff` 清成 0：

```c
old_pgoff = vma->vm_pgoff;
vma->vm_pgoff = 0;
ret = dma_mmap_coherent(ctx->dev, vma, cpu_addr, dma_addr, ctx->size);
vma->vm_pgoff = old_pgoff;
```

理解方式：

```text
用户传入的 mmap offset：
    驱动自己用来选择 src/dst。

传给 dma_mmap_coherent 时：
    已经选中某一块 buffer，所以应该从这块 buffer 的 offset 0 开始映射。
```

## mmap 回调里的检查

建议保留这些检查：

```c
if (!ctx->size || !ctx->src_cpu_addr || !ctx->dst_cpu_addr)
    return -ENOMEM;
```

表示用户必须先 `ioctl ALLOC`，再 `mmap`。

```c
if (req_size > ctx->size)
    return -EINVAL;
```

表示一次 mmap 不能超过单块 buffer 大小。

```c
if (offset 既不是 src offset，也不是 dst offset)
    return -EINVAL;
```

表示用户传了非法 offset。

## mmap 和 DMA cache 的关系

当前模板用的是：

```c
dma_alloc_coherent()
dma_mmap_coherent()
```

coherent buffer 一般不需要你手动调用：

```c
dma_map_single()
dma_sync_single_for_device()
dma_sync_single_for_cpu()
dma_unmap_single()
```

它适合先学习：

```text
用户态虚拟地址
内核虚拟地址
DMA 地址
mmap 生命周期
ioctl 控制流程
```

后面如果改成 streaming DMA 或普通 page buffer，就要重新考虑 cache 同步问题。

## 常见错误

`mmap failed: No such device or address`

常见原因：

```text
1. dst mmap 时没有清 vma->vm_pgoff，导致 dma_mmap_coherent 认为越界。
2. 用户态 offset 没有页对齐。
3. 用户态 length 超过 buffer size。
4. 没有先 ioctl ALLOC 就 mmap。
```

`mmap failed: Invalid argument`

常见原因：

```text
1. offset 既不是 0，也不是 PAGE_ALIGN(size)。
2. length 参数不合法。
3. 用户态忘了用 MAP_SHARED。
```

用户态能 mmap，但数据不对：

```text
1. 映射 src/dst 的 offset 写反了。
2. 用户态写的是 src，但检查了错误的地址。
3. DMA_TEST_IOC_START 还没真正接 DMA Engine。
4. 如果换成 streaming DMA，可能是 cache sync 时机不对。
```

## 推荐记忆方式

把 mmap 记成这句话：

```text
mmap 不是复制数据，而是让用户态虚拟地址映射到驱动管理的那块 buffer。
```

再记住三类地址：

```text
用户态虚拟地址：app 用
内核虚拟地址：驱动用
DMA 地址：硬件用
```

最后记住 offset 的两层含义：

```text
驱动层：
    offset 用来选择 src 或 dst。

dma_mmap_coherent 层：
    vma->vm_pgoff 表示当前 buffer 内部偏移。
```

所以选择完 src/dst 后，调用 `dma_mmap_coherent()` 前要把 `vma->vm_pgoff` 归零。
