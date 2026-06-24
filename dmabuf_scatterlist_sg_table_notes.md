# DMA-BUF scatterlist / sg_table 笔记

这份笔记用来配合 `drivers/media/simple_dmabuf_exporter.c` 看。重点不是背结构体字段，而是搞清楚：

```text
dmabuf fd
  -> struct dma_buf
  -> dma_buf_attachment
  -> sg_table
  -> scatterlist entries
  -> dma_map_sg 后的设备 DMA 地址
```

## 1. scatterlist 是什么

`struct scatterlist` 是 SG 表的一项。SG 是 scatter-gather，意思是：一个逻辑上的 buffer，背后可能由多段物理内存组成，DMA 设备可以按一张表逐段访问。

一个 `scatterlist` entry 描述一段内存片段：

```text
page_link    指向 struct page，低位还编码 SG_CHAIN/SG_END 标志
offset       从这个 page 内哪个偏移开始
length       CPU/page 视角下这一段多长
dma_address  dma_map_sg 后设备看到的 DMA 地址
dma_length   dma_map_sg 后设备看到的长度，部分架构才有
```

不要直接操作 `page_link`、`dma_address`、`dma_length`。常用 API 是：

```c
sg_set_page(sg, page, len, offset);
sg_page(sg);
sg_next(sg);
sg_dma_address(sg);
sg_dma_len(sg);
```

## 2. 它不是普通 list_head 链表

`scatterlist` 通常是一段数组：

```text
sg[0]
sg[1]
sg[2]
```

数组最后一项用 `SG_END` 标记。如果 entry 很多，内核可以用一个特殊 chain entry 链到下一组 scatterlist 数组：

```text
sg array A
  A[0] -> page
  A[1] -> page
  A[2] -> chain to B

sg array B
  B[0] -> page
  B[1] -> END
```

所以它的“链”不是把数据 page 一个个用 `next` 串起来，而是在需要时把多组 sg 数组串起来。遍历时用 `sg_next()` 或 `for_each_sg()`，不要自己假设 `sg++` 永远正确。

## 3. sg_table 是什么

实际工程里更常见的是：

```c
struct sg_table {
	struct scatterlist *sgl;
	unsigned int nents;
	unsigned int orig_nents;
};
```

含义：

```text
sgl        第一个 scatterlist entry
orig_nents 原始 entry 数量，也就是 page/CPU 视角的段数
nents      dma_map_sg 后的有效 DMA entry 数量
```

这个区别非常重要。`dma_map_sg()` 可能把原本相邻的 DMA 段合并，所以：

```text
CPU/page 视角遍历：用 orig_nents
DMA/device 视角遍历：用 nents
```

内核里也有对应宏：

```c
for_each_sgtable_sg(sgt, sg, i)      // 用 orig_nents
for_each_sgtable_dma_sg(sgt, sg, i)  // 用 nents
```

## 4. dma_map_sg 前后有什么变化

在 `dma_map_sg()` 之前，sg entry 主要描述的是：

```text
struct page + offset + length
```

这属于 CPU/page 视角。

调用：

```c
nents = dma_map_sg(dev, sgt->sgl, sgt->orig_nents, DMA_TO_DEVICE);
```

之后，设备应该使用：

```c
sg_dma_address(sg);
sg_dma_len(sg);
```

这属于 device/DMA 视角。

注意：`dma_unmap_sg()` 需要传入原始 entry 数量，也就是 map 时传入的数量。用 `struct sg_table` 的辅助函数更不容易弄错：

```c
ret = dma_map_sgtable(dev, sgt, dir, attrs);
dma_unmap_sgtable(dev, sgt, dir, attrs);
```

`dma_map_sgtable()` 内部会：

```text
用 sgt->orig_nents 调 dma_map_sg_attrs()
把返回的 mapped nents 写入 sgt->nents
```

`dma_unmap_sgtable()` 内部会：

```text
用 sgt->orig_nents 调 dma_unmap_sg_attrs()
```

## 5. DMA-BUF 里的 exporter/importer 视角

DMA-BUF exporter 负责创建 backing storage，并在 `map_dma_buf()` 回调里返回 `sg_table`。

importer 路径一般是：

```c
dmabuf = dma_buf_get(fd);
attach = dma_buf_attach(dmabuf, importer_dev);
sgt = dma_buf_map_attachment(attach, DMA_TO_DEVICE);

for_each_sgtable_dma_sg(sgt, sg, i) {
	dma_addr_t addr = sg_dma_address(sg);
	unsigned int len = sg_dma_len(sg);
	/* 把 addr/len 配置给硬件 */
}

dma_buf_unmap_attachment(attach, sgt, DMA_TO_DEVICE);
dma_buf_detach(dmabuf, attach);
dma_buf_put(dmabuf);
```

所以 importer 不应该直接拿 exporter 内部的 `buf->dma_addr`。它应该拿 `dma_buf_map_attachment()` 返回的 `sg_table`，再从 DMA-mapped sg entry 中取 `sg_dma_address()` / `sg_dma_len()`。

## 6. 对 simple_dmabuf_exporter.c 的修正点

这个教学 exporter 的 backing storage 来自：

```c
dma_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);
```

早期注释容易让人误解成 exporter 可以自己先填：

```c
sg_dma_address(sg) = buf->dma_addr;
sg_dma_len(sg) = buf->size;
```

这个理解不严谨。更好的顺序是：

```text
1. exporter 先用 dma_get_sgtable_attrs() 从 coherent backing storage 得到 sg_table
2. sg_table 此时描述 page/CPU 视角的 backing storage
3. 再用 dma_map_sgtable(attach->dev, sgt, direction, attrs)
4. map 成 importer 设备可用的 DMA 地址
5. importer 再用 sg_dma_address()/sg_dma_len()
```

一句话记：

```text
sg_set_page / dma_get_sgtable_attrs：描述内存在哪里
dma_map_sg / dma_map_sgtable：把这张内存表映射成某个设备能用的 DMA 地址
sg_dma_address / sg_dma_len：只能在 DMA map 之后给设备使用
```

## 7. 和你现在实验的关系

如果只是 `dmaengine_prep_dma_memcpy(chan, dst_dma, src_dma, len, flags)`，两边都是连续 DMA 地址，那暂时不需要直接操作 sg_table。

但真实 V4L2、RGA、MPP、DRM、dma-buf import/export 经常会绕到 `sg_table`，因为 buffer 背后可能是：

```text
CMA 连续内存
IOMMU 映射后的 IOVA
多个 page 拼起来的大 buffer
dma-heap / ION / vb2 导出的 dmabuf
```

看到 `sg_table` 时，先把它理解成：

```text
这个 buffer 背后的内存分布图；
经过 dma_map_sgtable 后，它才变成某个 importer 设备可用的 DMA 地址列表。
```
