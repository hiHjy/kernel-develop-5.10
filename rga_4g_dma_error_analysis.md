# RGA `unsupported memory larger than 4G` 问题分析

## 1. 现象

项目里使用 RGA 处理 dmabuf 时，内核 dmesg 可能出现：

```text
RGA_MMU unsupported memory larger than 4G!
scheduler core[x] unsupported mm_flag[...]
rga_mm_map_dma_buffer core[x] map dma buffer error!
rga_mm_map_buffer map dma_buf error!
```

用户态通常只看到：

```text
RgaBlit failed
Invalid argument
ret = -22
```

真正根因要看内核 dmesg。

## 2. 从 fd 导入开始的路径

应用层把 dmabuf fd 传给 RGA 后，大致路径是：

```text
RGA_DMA_BUFFER
  -> rga_mm_map_buffer()
  -> rga_mm_map_dma_buffer()
  -> rga_dma_map_fd()
  -> dma_buf_get(fd)
  -> dma_buf_attach(dma_buf, rga_dev)
  -> dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL)
  -> 得到 sg_table
  -> rga_mm_check_range_sgt()
  -> rga_mm_check_memory_limit()
```

关键源码：

```c
/* drivers/video/rockchip/rga3/rga_dma_buf.c */
dma_buf = dma_buf_get(fd);
attach = dma_buf_attach(dma_buf, rga_dev);
sgt = dma_buf_map_attachment(attach, dir);
rga_dma_buffer->dma_addr = sg_dma_address(sgt->sgl);
```

然后 RGA 检查 sg 真实物理地址：

```c
/* drivers/video/rockchip/rga3/rga_mm.c */
if (rga_mm_check_range_sgt(buffer->sgt))
	mm_flag |= RGA_MEM_UNDER_4G;
```

检查函数：

```c
s_phys = sg_phys(sg);
if ((s_phys > 0xffffffff) || (s_phys + sg->length > 0xffffffff))
	return 0;
```

如果当前 scheduler 是 `RGA_MMU`，并且没有 `RGA_MEM_UNDER_4G`，就报错：

```c
if (scheduler->data->mmu == RGA_MMU &&
    !(mm_flag & RGA_MEM_UNDER_4G)) {
	pr_err("%s unsupported memory larger than 4G!\n",
	       rga_get_mmu_type_str(scheduler->data->mmu));
	return false;
}
```

## 3. 两条地址路径

这里一定要区分两个地址：

```text
sg_phys(sg)
  真实物理地址 / CPU 物理地址视角

sg_dma_address(sg)
  DMA API map 后给设备看的地址
  如果设备走 IOMMU，这个地址可能是 IOVA
```

也就是说：

```text
sg_phys(sg)        用来检查真实物理页是否在 4G 以下
sg_dma_address(sg) 才是设备最终拿去 DMA 的地址
```

RGA 的两条路：

```text
RGA_MMU 路径：
  受真实物理地址限制
  要求 buffer 的 sg_phys 在 4G 以下
  超过 4G 会报 unsupported memory larger than 4G

RGA_IOMMU 路径：
  可以通过 IOMMU 把物理地址映射成 IOVA
  硬件使用 sg_dma_address()/IOVA
  不应该因为真实物理地址超过 4G 触发这个 RGA_MMU 检查
```

所以这个错误通常说明：

```text
当前任务被调度到了 RGA_MMU 类型 core/path
并且导入 dmabuf 背后的真实物理地址超过 4G
```

## 4. 为什么同一芯片有的设备没问题

同一颗 RK3568 上，不同硬件模块的 DMA 能力不同：

```text
有的设备有标准 IOMMU
有的驱动会走 IOVA
有的设备只能访问 32-bit 物理地址
有的分配器会保证 buffer 在 4G 以下
RGA 驱动内部还会区分 RGA_MMU / RGA_IOMMU
```

RGA 驱动里设置 mask 的地方：

```c
/* drivers/video/rockchip/rga3/rga_drv.c */
if (scheduler->data->mmu == RGA_IOMMU) {
	dma_set_mask(dev, DMA_BIT_MASK(40));
	dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
} else {
	dma_set_mask(dev, DMA_BIT_MASK(32));
	dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
}
```

含义：

```text
RGA_IOMMU:
  streaming DMA 可以到 40bit
  coherent DMA 还是 32bit

RGA_MMU:
  streaming/coherent 都按 32bit
```

## 5. 常见根因

这个错误一般不是用户态参数简单写错，而是下面几类问题：

```text
1. dmabuf exporter 分配到了 4G 以上物理内存
2. RGA 当前调度到了 RGA_MMU 路径，而不是 RGA_IOMMU 路径
3. 使用的 heap/CMA 没有限制到 4G 以下
4. RGA 驱动/调度策略没有自动避开不支持 4G 以上地址的 core
5. 老版本 librga/kernel rga driver 对 dmabuf/IOMMU 调度处理不完善
```

## 6. 解决方向

### 方案一：让 buffer 分配在 4G 以下

这是最直接、最稳的办法。

可以考虑：

```text
使用 4G 以下 CMA
使用受限 dma-heap / CMA heap
设备树 reserved-memory 限制分配区域
避免从可能分配到高地址的 heap 申请给 RGA_MMU 用的 buffer
```

目标是让：

```c
sg_phys(sg) <= 0xffffffff
sg_phys(sg) + sg->length <= 0xffffffff
```

这样 `rga_mm_check_range_sgt()` 会置上：

```c
RGA_MEM_UNDER_4G
```

RGA_MMU 路径就不会拒绝。

### 方案二：确保走 RGA_IOMMU 路径

如果硬件和驱动支持，应尽量让 RGA 使用 IOMMU/IOVA 路径。

可以检查：

```text
当前 RGA core 的 scheduler->data->mmu 是 RGA_MMU 还是 RGA_IOMMU
设备树 RGA/IOMMU 节点是否启用
驱动版本是否支持对应 RGA core 的 IOMMU
librga 是否能选择/调度到支持 IOMMU 的 core
```

如果走 `RGA_IOMMU`：

```text
真实物理地址可以超过 4G
设备使用 IOVA
sg_dma_address(sg) 才是给硬件用的地址
```

### 方案三：升级 librga 和 kernel RGA driver

升级有可能解决，但不是万能。

新版本可能改进：

```text
RGA core 调度策略
dmabuf import 路径
IOMMU 使用策略
高地址 buffer fallback
dma-heap 兼容性
错误提示
```

但如果某个 RGA_MMU core 硬件本身只能处理 4G 以下物理地址，升级驱动也不能让它凭空支持高地址。驱动只能：

```text
避开这个 core
走 RGA_IOMMU
或者要求 buffer 分配在 4G 以下
```

## 7. 排查建议

遇到这个问题时，优先查：

```text
1. dmesg 是否有 unsupported memory larger than 4G
2. 当前任务调度到哪个 RGA core
3. scheduler->data->mmu 是 RGA_MMU 还是 RGA_IOMMU
4. dmabuf 的 sg_phys 是否超过 0xffffffff
5. dmabuf 来自哪个 heap / exporter
6. RGA 驱动和 librga 版本
```

可以临时加日志：

```c
for_each_sg(sgt->sgl, sg, sgt->orig_nents, i) {
	pr_info("sg[%d] phys=%pa len=%u dma=%pad dma_len=%u\n",
		i, &sg_phys(sg), sg->length,
		&sg_dma_address(sg), sg_dma_len(sg));
}
```

看清楚：

```text
phys 是否超过 4G
dma 是否是 IOVA
当前路径是否还在使用 RGA_MMU 限制
```

## 8. 一句话总结

```text
unsupported memory larger than 4G 不是 dmabuf fd 本身错了，
而是 RGA_MMU 路径不能处理这个 dmabuf 背后的高物理地址。

要么让 buffer 分配到 4G 以下，
要么确保 RGA 走 IOMMU/IOVA 路径，
要么升级/调整 RGA 驱动和 librga 的调度策略。
```
