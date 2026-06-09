# virtual_video_dmabuf_drv DMA 改造方向

目标文件：

```text
drivers/media/virtual_video_dmabuf_drv .c
```

当前驱动是一个虚拟 V4L2 capture 设备。用户态通过 `REQBUFS / MMAP / QBUF / STREAMON / DQBUF` 取帧，驱动内部用 timer 周期性产生帧。

现在的数据路径是：

```text
g_test_frames[]                 vb2_vmalloc buffer
vmalloc 内存                    vmalloc/mmap buffer
     |                                ^
     | CPU memcpy                     |
     +--------------------------------+

timer_function()
  -> vb2_plane_vaddr()
  -> memcpy()
  -> vb2_buffer_done()
```

你要改的不是“把 mmap 删除”，而是把“填充 vb2 buffer 的方式”从 CPU memcpy 换成 DMA memcpy。

改造后的目标路径是：

```text
g_test_frames[]                 vb2_dma_contig buffer
DMA coherent 源 buffer           DMA contiguous 目标 buffer
     |                                ^
     | PL330 / dmaengine memcpy       |
     +--------------------------------+

timer/work
  -> 取 queued buffer
  -> vb2_dma_contig_plane_dma_addr()
  -> dmaengine_prep_dma_memcpy()
  -> DMA complete callback
  -> vb2_buffer_done()
```

用户态仍然可以继续用：

```text
v4l2-ctl --stream-mmap
```

因为 `mmap` 是用户态访问 V4L2 buffer 的方式，DMA 是驱动内部模拟硬件写帧的方式。

## 当前代码的关键点

当前使用的是：

```c
#include <media/videobuf2-vmalloc.h>

g_vb_queue.mem_ops = &vb2_vmalloc_memops;
```

timer 中直接取 CPU 虚拟地址：

```c
ptr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
memcpy(ptr, img, 800 * 600 * 2);
```

这条路适合学习 V4L2/vb2 流程，但不适合模拟真实 DMA。

原因是：

```text
vb2_vmalloc_memops 分配的是虚拟连续内存。
PL330/DMA 控制器需要 DMA 地址。
vmalloc 得到的 CPU 虚拟地址不能直接给 DMA 控制器使用。
```

## 总体改造步骤

推荐按这个顺序改，不要一口气全塞进去：

```text
1. 把驱动挂到一个真实 struct device 上
2. vb2_vmalloc_memops 换成 vb2_dma_contig_memops
3. 测试帧源 buffer 从 vmalloc 换成 dma_alloc_coherent
4. 申请 PL330 dmaengine memcpy channel
5. timer 只负责触发取 buffer，实际 DMA 提交放到 workqueue
6. 用 DMA memcpy 把源帧搬到 vb2 buffer
7. DMA 完成 callback 里调用 vb2_buffer_done
8. stop_streaming 中终止 DMA 并归还所有 buffer
```

## 第一步：需要真实 struct device

当前代码里：

```c
v4l2_device_register(NULL, &v4l2_dev);
```

这对纯虚拟 `vb2_vmalloc_memops` 驱动没问题，但切到 DMA 后不合适。

`vb2_dma_contig_memops`、`dma_alloc_coherent()`、`dma_request_chan()` 都需要围绕 `struct device *dev` 工作。

建议把驱动改成 platform driver：

```c
struct virtual_video_dev {
	struct device *dev;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue vb_queue;
	struct dma_chan *dma_chan;
	struct work_struct dma_work;
	spinlock_t queued_lock;
	struct list_head queued_bufs;
};
```

probe 中保存：

```c
vdev->dev = &pdev->dev;
```

然后：

```c
ret = v4l2_device_register(vdev->dev, &vdev->v4l2_dev);

vdev->vb_queue.dev = vdev->dev;
vdev->vb_queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
vdev->vb_queue.io_modes = VB2_MMAP;
vdev->vb_queue.mem_ops = &vb2_dma_contig_memops;
```

早期调通阶段建议先只开：

```c
VB2_MMAP
```

暂时不要开：

```c
VB2_USERPTR
VB2_DMABUF
VB2_READ
```

原因：

```text
VB2_MMAP 最适合先验证 DMA 写入 vb2 buffer。
USERPTR 需要 pin/map 用户页。
DMABUF 涉及 importer/exporter 路径。
READ 会多一条拷贝路径，容易干扰判断。
```

## 第二步：换成 vb2_dma_contig_memops

include 替换：

```c
-#include <media/videobuf2-vmalloc.h>
+#include <media/videobuf2-dma-contig.h>
```

队列初始化替换：

```c
-g_vb_queue.mem_ops = &vb2_vmalloc_memops;
+g_vb_queue.mem_ops = &vb2_dma_contig_memops;
```

并补上：

```c
g_vb_queue.dev = dev;
```

这样 vb2 给用户态分配的 buffer 就是 DMA contiguous buffer，驱动可以拿到 DMA 地址：

```c
dma_addr_t dst_dma;

dst_dma = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
```

## 第三步：测试帧源 buffer 也要 DMA 可访问

当前测试帧是：

```c
static unsigned char *g_test_frames[VIRTUAL_FRAME_NUM];

g_test_frames[i] = vmalloc(VIRTUAL_FRAME_SIZE);
```

要给 PL330 当 DMA 源，建议改成：

```c
struct virtual_dma_frame {
	void *cpu;
	dma_addr_t dma;
};

static struct virtual_dma_frame g_test_frames[VIRTUAL_FRAME_NUM];
```

初始化：

```c
g_test_frames[i].cpu = dma_alloc_coherent(dev,
					  VIRTUAL_FRAME_SIZE,
					  &g_test_frames[i].dma,
					  GFP_KERNEL);
if (!g_test_frames[i].cpu)
	return -ENOMEM;
```

填颜色时：

```c
fill_yuyv_frame(g_test_frames[0].cpu, 76, 84, 255);
fill_yuyv_frame(g_test_frames[1].cpu, 150, 44, 21);
fill_yuyv_frame(g_test_frames[2].cpu, 29, 255, 107);
```

释放：

```c
dma_free_coherent(dev,
		  VIRTUAL_FRAME_SIZE,
		  g_test_frames[i].cpu,
		  g_test_frames[i].dma);
```

这样源地址就有两种视角：

```text
CPU 填测试图：g_test_frames[i].cpu
PL330 读源帧：g_test_frames[i].dma
```

## 第四步：申请 PL330 DMA channel

如果你的 DTS 已经给设备配置了 DMA，可以走：

```c
vdev->dma_chan = dma_request_chan(dev, "rx");
if (IS_ERR(vdev->dma_chan))
	return PTR_ERR(vdev->dma_chan);
```

如果你只是学习验证，也可以先用 `dma_request_channel()` 按 capability 找 memcpy channel。

需要的 include：

```c
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
```

memcpy 能力检查方向：

```c
dma_cap_mask_t mask;

dma_cap_zero(mask);
dma_cap_set(DMA_MEMCPY, mask);
```

真实项目里更推荐 DTS + `dma_request_chan()`，因为这样 channel 归属更清楚。

## 第五步：timer 不要直接提交复杂 DMA

当前 `timer_function()` 在软中断上下文运行。

现在它做 `memcpy()` 问题不大，但换成 dmaengine 后，建议不要在 timer 里做太多事情。

推荐结构：

```text
timer_function()
  -> schedule_work(&vdev->dma_work)

virtual_dma_work()
  -> 从 queued_bufs 取一个 buffer
  -> 提交 dmaengine memcpy
```

原因：

```text
workqueue 是进程上下文，更适合调用可能睡眠或较复杂的 DMA 提交流程。
timer 只作为周期性触发器。
```

## 第六步：提交 DMA memcpy

work 中大概是：

```c
static void virtual_dma_work(struct work_struct *work)
{
	struct virtual_video_dev *vdev =
		container_of(work, struct virtual_video_dev, dma_work);
	struct virtual_frame_buf *buf;
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	dma_addr_t src_dma;
	dma_addr_t dst_dma;
	unsigned long flags;

	spin_lock_irqsave(&vdev->queued_lock, flags);
	if (list_empty(&vdev->queued_bufs)) {
		spin_unlock_irqrestore(&vdev->queued_lock, flags);
		return;
	}

	buf = list_first_entry(&vdev->queued_bufs,
			       struct virtual_frame_buf, list);
	list_del_init(&buf->list);
	spin_unlock_irqrestore(&vdev->queued_lock, flags);

	src_dma = vdev->test_frames[vdev->frame_index].dma;
	dst_dma = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);

	desc = dmaengine_prep_dma_memcpy(vdev->dma_chan,
					 dst_dma,
					 src_dma,
					 VIRTUAL_FRAME_SIZE,
					 DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc) {
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		return;
	}

	desc->callback = virtual_dma_complete;
	desc->callback_param = buf;

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie)) {
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		return;
	}

	dma_async_issue_pending(vdev->dma_chan);
}
```

注意参数顺序：

```c
dmaengine_prep_dma_memcpy(chan, dst_dma, src_dma, len, flags)
```

不是 `src, dst`。

## 第七步：DMA 完成回调里归还 buffer

完成回调大概是：

```c
static void virtual_dma_complete(void *param)
{
	struct virtual_frame_buf *buf = param;

	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, VIRTUAL_FRAME_SIZE);
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}
```

如果后面需要访问设备私有结构，比如更新 `frame_index`、判断 streaming 状态，建议让 callback 参数传一个小上下文：

```c
struct virtual_dma_desc_ctx {
	struct virtual_video_dev *vdev;
	struct virtual_frame_buf *buf;
};
```

第一版为了简单，可以先保证同一时间只飞一个 DMA。

## 第八步：限制同一时间只跑一个 DMA

刚开始不要让多个 buffer 同时提交到同一个 PL330 channel。

可以加一个状态：

```c
bool dma_busy;
```

work 里：

```c
if (vdev->dma_busy)
	return;

vdev->dma_busy = true;
```

callback 里：

```c
vdev->dma_busy = false;
schedule_work(&vdev->dma_work);
```

这样如果队列里还有 buffer，DMA 完成后继续搬下一帧。

## 第九步：stop_streaming 要处理正在 DMA 的 buffer

当前 `stop_streaming()` 做了两件事：

```text
del_timer_sync()
把 queued list 中的 buffer 全部 VB2_BUF_STATE_ERROR 归还
```

DMA 版还要多处理：

```text
停止 timer
取消 work
终止 DMA channel
归还正在 DMA 的 active buffer
归还 queued list 中还没 DMA 的 buffer
```

大概结构：

```c
del_timer_sync(&vdev->timer);
cancel_work_sync(&vdev->dma_work);

if (vdev->dma_chan)
	dmaengine_terminate_sync(vdev->dma_chan);

if (vdev->active_buf) {
	vb2_buffer_done(&vdev->active_buf->vb.vb2_buf,
			VB2_BUF_STATE_ERROR);
	vdev->active_buf = NULL;
}

list_for_each_entry_safe(buf, tmp, &vdev->queued_bufs, list) {
	list_del_init(&buf->list);
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
}
```

vb2 对 `stop_streaming()` 有硬性要求：

```text
stop_streaming 返回前，所有属于驱动的 buffer 都必须还给 vb2。
```

否则会报类似：

```text
driver bug: stop_streaming operation is leaving buf ... in active state
```

## 第十步：buffer 状态建议

建议在设备私有结构里维护：

```c
struct virtual_video_dev {
	...
	struct timer_list timer;
	struct work_struct dma_work;
	struct dma_chan *dma_chan;

	spinlock_t qlock;
	struct list_head queued_bufs;
	struct virtual_frame_buf *active_buf;
	bool streaming;
	bool dma_busy;

	struct virtual_dma_frame test_frames[VIRTUAL_FRAME_NUM];
	unsigned int frame_index;
};
```

状态流转：

```text
QBUF
  -> virtual_buf_queue()
  -> queued_bufs

timer
  -> schedule_work()

work
  -> queued_bufs 取出一个
  -> active_buf = buf
  -> dma_busy = true
  -> submit DMA

DMA callback
  -> payload
  -> vb2_buffer_done(DONE)
  -> active_buf = NULL
  -> dma_busy = false
```

## 关于 cache coherency

第一版建议：

```text
源测试帧使用 dma_alloc_coherent()
目标 vb2 buffer 使用 vb2_dma_contig_memops
```

这样 cache 问题最少。

如果后面源 buffer 换成普通 kmalloc/vmalloc 或用户 buffer，就要考虑：

```text
dma_map_single()
dma_unmap_single()
dma_sync_single_for_device()
dma_sync_single_for_cpu()
```

但第一版不要把这些复杂度引进来。

## 和 dmabuf 的关系

这次改造可以先不碰 dmabuf。

当前目标是：

```text
V4L2 MMAP buffer 由 PL330 DMA 写入
```

等这个跑通后，再考虑：

```text
V4L2 buffer export 成 dmabuf fd
或者 import 外部 dmabuf fd
```

两者层级不同：

```text
DMA memcpy
    解决当前驱动如何把数据搬到 buffer。

dmabuf
    解决这块 buffer 如何跨驱动共享。
```

## 最小验证路径

第一版跑通后，用：

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoX --all
v4l2-ctl -d /dev/videoX --stream-mmap=3 --stream-count=30 --stream-to=out.yuyv
```

检查：

```text
DQBUF 是否正常返回
out.yuyv 大小是否等于 frame_count * 800 * 600 * 2
画面颜色是否按三帧循环变化
dmesg 是否没有 vb2 active buffer 泄漏警告
```

如果要看 YUYV：

```bash
ffplay -f rawvideo -pixel_format yuyv422 -video_size 800x600 out.yuyv
```

## 常见坑

### 1. 继续用 vb2_vmalloc_memops

错误方向：

```text
vb2_vmalloc buffer + PL330 DMA
```

除非你自己把 vmalloc 页拆成 sg，然后映射成 DMA 地址，否则不要这么走。

学习阶段直接换 `vb2_dma_contig_memops`。

### 2. 没有给 vb2_queue 设置 dev

DMA contig 需要：

```c
g_vb_queue.dev = dev;
```

否则分配和 DMA 相关路径会出问题。

### 3. 在 timer 里做复杂 DMA 提交

timer 是软中断上下文。

建议：

```text
timer -> schedule_work -> work 提交 DMA
```

### 4. DMA callback 里忘记 vb2_buffer_done

用户态 `DQBUF` 能返回，靠的是：

```c
vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
```

如果 DMA 完成了但没有调用它，用户态会一直卡在取帧。

### 5. stop_streaming 漏还 active buffer

只清 queued list 不够。

DMA 版还有一个“已经从队列取出，正在 DMA”的 buffer。

它也必须在 stop 时归还。

### 6. dmaengine_prep_dma_memcpy 参数顺序写反

正确是：

```c
dmaengine_prep_dma_memcpy(chan, dst, src, len, flags)
```

### 7. 过早打开 VB2_DMABUF

`VB2_DMABUF` 是另一条路。

先把 MMAP + DMA 写帧跑通，再加 dmabuf，会更清晰。

## 推荐第一版目标

第一版只做到：

```text
platform_driver
  +
vb2_dma_contig_memops
  +
dma_alloc_coherent 源测试帧
  +
PL330 DMA memcpy
  +
v4l2-ctl --stream-mmap 能抓到变化的 YUYV
```

这一步跑通，就说明你已经把虚拟摄像头从“CPU memcpy 产帧”改成了“DMA 模拟硬件写帧”。

