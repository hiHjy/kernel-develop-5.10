# V4L2/vb2 Buffer Flow Notes

这份笔记按虚拟摄像头驱动的视角，梳理应用层 buffer 如何进入驱动、驱动如何填帧、最后如何返回应用层。

## 一句话主线

```text
应用层 QBUF 空 buffer
  -> vb2 准备并管理 buffer
  -> vb2 调驱动 .buf_queue()
  -> 驱动填图像
  -> 驱动调用 vb2_buffer_done(DONE)
  -> 应用层 DQBUF 取回满 buffer
```                                       

## 关键结构

你的自定义 buffer：

```c
struct virtual_frame_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};
```

含义：

```text
vb
  给 V4L2/vb2 框架使用，里面包含 struct vb2_buffer。

list
  给你的驱动自己使用，用来挂到 g_queued_bufs 待填充队列。
```

真实嵌套关系：

```text
struct virtual_frame_buf
└── struct vb2_v4l2_buffer vb
    └── struct vb2_buffer vb2_buf
```

vb2 回调传进来的 `struct vb2_buffer *vb`，其实就是：

```c
&buf->vb.vb2_buf
```

所以驱动里可以这样反推出自己的完整 buffer：

```c
struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
struct virtual_frame_buf *buf =
	container_of(vbuf, struct virtual_frame_buf, vb);
```

这个能成立的关键配置是：

```c
g_vb_queue.buf_struct_size = sizeof(struct virtual_frame_buf);
```

它告诉 vb2：每个 buffer 按你的 `struct virtual_frame_buf` 大小分配。

## 三个队列

vb2 和驱动里会同时出现多个链表，不要混在一起。

```text
q->queued_list
  vb2 内部队列。
  保存所有应用层已经 QBUF、还没 DQBUF 的 buffer。
  使用节点：vb->queued_entry

g_queued_bufs
  驱动自己的队列。
  保存已经交给驱动、等待你填图像的 ACTIVE buffer。
  使用节点：buf->list

q->done_list
  vb2 内部完成队列。
  保存驱动已经填好、等待应用层 DQBUF 的 buffer。
  使用节点：vb->done_entry
```

所以这三个链表节点分别是：

```text
vb->queued_entry  -> q->queued_list
buf->list         -> g_queued_bufs
vb->done_entry    -> q->done_list
```

## QBUF 入口

应用层调用：

```c
ioctl(fd, VIDIOC_QBUF, &buf);
```

进入：

```c
vb2_ioctl_qbuf()
  -> vb2_qbuf()
  -> vb2_core_qbuf()
```

源码位置：

```text
drivers/media/common/videobuf2/videobuf2-v4l2.c
drivers/media/common/videobuf2/videobuf2-core.c
```

`vb2_ioctl_qbuf()` 会通过 `video_device` 找到队列：

```c
struct video_device *vdev = video_devdata(file);
return vb2_qbuf(vdev->queue, vdev->v4l2_dev->mdev, p);
```

你的驱动里对应的是：

```c
virtual_video.queue = &g_vb_queue;
```

所以应用层 QBUF 最终进入你的 `g_vb_queue`。

## prepared 什么时候设置

在 `vb2_core_qbuf()` 里：

```c
vb = q->bufs[index];

if (!vb->prepared) {
	ret = __buf_prepare(vb);
	if (ret)
		return ret;
}
```

`__buf_prepare()` 里会做：

```c
vb->state = VB2_BUF_STATE_PREPARING;

switch (q->memory) {
case VB2_MEMORY_MMAP:
	ret = __prepare_mmap(vb);
	break;
case VB2_MEMORY_USERPTR:
	ret = __prepare_userptr(vb);
	break;
case VB2_MEMORY_DMABUF:
	ret = __prepare_dmabuf(vb);
	break;
}

__vb2_buf_mem_prepare(vb);
vb->prepared = 1;
vb->state = orig_state;
```

所以：

```text
prepared = 1
  表示这个 buffer 已经完成 QBUF 前的准备，可以交给驱动。
```

驱动可以提供可选回调：

```c
int (*buf_prepare)(struct vb2_buffer *vb);
```

常用于检查：

```text
buffer 大小是否足够
plane 数量是否正确
格式/stride 是否满足硬件要求
```

## synced 和 cache 同步

`__buf_prepare()` 会调用：

```c
__vb2_buf_mem_prepare(vb);
```

里面大概是：

```c
if (vb->synced)
	return;

if (vb->need_cache_sync_on_prepare) {
	for each plane:
		mem_ops->prepare(...);
}

vb->synced = 1;
```

含义：

```text
synced = 1
  表示该 buffer 已经做过内存 prepare。
```

后面 `vb2_buffer_done(DONE)` 时会调用：

```c
__vb2_buf_mem_finish(vb);
```

里面大概是：

```c
if (!vb->synced)
	return;

if (vb->need_cache_sync_on_finish) {
	for each plane:
		mem_ops->finish(...);
}

vb->synced = 0;
```

所以：

```text
mem_prepare -> synced = 1
mem_finish  -> synced = 0
```

这套机制和 cache 同步有关，不是只给 DMABUF 用。MMAP、USERPTR、DMABUF 都可能走 mem_ops。

但这份源码里对 DMABUF 有特殊处理：

```c
if (q->memory == VB2_MEMORY_DMABUF) {
	vb->need_cache_sync_on_finish = 0;
	vb->need_cache_sync_on_prepare = 0;
	return;
}
```

意思是 DMABUF exporter 通常应该自己负责 cache sync，vb2 这里默认不显式做。

## QBUF 后进入 vb2 queued_list

`vb2_core_qbuf()` 里：

```c
list_add_tail(&vb->queued_entry, &q->queued_list);
q->queued_count++;
vb->state = VB2_BUF_STATE_QUEUED;
```

此时：

```text
buffer 已经被应用层 QBUF
buffer 在 vb2 的 q->queued_list
state = QUEUED
```

注意：这还不是你的 `g_queued_bufs`。

## 什么时候调用驱动 .buf_queue()

如果已经 STREAMON：

```c
if (q->start_streaming_called)
	__enqueue_in_driver(vb);
```

`__enqueue_in_driver()`：

```c
static void __enqueue_in_driver(struct vb2_buffer *vb)
{
	struct vb2_queue *q = vb->vb2_queue;

	vb->state = VB2_BUF_STATE_ACTIVE;
	atomic_inc(&q->owned_by_drv_count);

	call_void_vb_qop(vb, buf_queue, vb);
}
```

也就是：

```text
state = ACTIVE
vb2 调你的 .buf_queue(vb)
```

你的驱动里：

```c
.buf_queue = virtual_buf_queue,
```

所以会进入：

```c
static void virtual_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct virtual_frame_buf *buf =
		container_of(vbuf, struct virtual_frame_buf, vb);

	list_add_tail(&buf->list, &g_queued_bufs);
}
```

这一步含义：

```text
vb2 已经把 ACTIVE buffer 交给驱动。
驱动把它加入自己的待填充队列 g_queued_bufs。
```

## 先 QBUF 后 STREAMON 的情况

如果应用层先 QBUF 多个 buffer，再 STREAMON，那么 QBUF 时只是进入 `q->queued_list`。

STREAMON 后，vb2 会在 `vb2_start_streaming()` 里做：

```c
list_for_each_entry(vb, &q->queued_list, queued_entry)
	__enqueue_in_driver(vb);

q->start_streaming_called = 1;
ret = call_qop(q, start_streaming, q,
	       atomic_read(&q->owned_by_drv_count));
```

所以不管顺序是：

```text
QBUF -> STREAMON
```

还是：

```text
STREAMON -> QBUF
```

最终都会走到：

```text
__enqueue_in_driver()
  -> virtual_buf_queue()
```

## timer 填帧

你的 timer 模拟硬件产生一帧图像。

典型逻辑：

```c
spin_lock_irqsave(&g_queued_bufs_lock, flags);
if (list_empty(&g_queued_bufs)) {
	spin_unlock_irqrestore(&g_queued_bufs_lock, flags);
	return;
}

buf = list_first_entry(&g_queued_bufs,
		       struct virtual_frame_buf,
		       list);
list_del_init(&buf->list);
spin_unlock_irqrestore(&g_queued_bufs_lock, flags);

ptr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
memcpy(ptr, img, VIRTUAL_FRAME_SIZE);

vb2_set_plane_payload(&buf->vb.vb2_buf, 0, VIRTUAL_FRAME_SIZE);
vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
```

这里分两步：

```text
list_del_init(&buf->list)
  从驱动自己的 g_queued_bufs 里取走这个待填充 buffer。

vb2_buffer_done(... DONE)
  告诉 vb2：这个 buffer 填好了，可以给应用层 DQBUF。
```

## vb2_buffer_done 做了什么

源码主线：

```c
void vb2_buffer_done(struct vb2_buffer *vb, enum vb2_buffer_state state)
{
	if (WARN_ON(vb->state != VB2_BUF_STATE_ACTIVE))
		return;

	if (state != VB2_BUF_STATE_QUEUED)
		__vb2_buf_mem_finish(vb);

	spin_lock_irqsave(&q->done_lock, flags);

	list_add_tail(&vb->done_entry, &q->done_list);
	vb->state = state;

	atomic_dec(&q->owned_by_drv_count);

	spin_unlock_irqrestore(&q->done_lock, flags);

	wake_up(&q->done_wq);
}
```

含义：

```text
要求 buffer 当前必须是 ACTIVE
调用 mem_finish，synced = 0
把 buffer 加入 q->done_list
state = DONE
驱动不再拥有它
唤醒等待 DQBUF/read/poll 的应用层
```

所以：

```text
vb2_buffer_done(DONE)
  就是驱动把满 buffer 还给 vb2。
```

## DQBUF 入口

应用层调用：

```c
ioctl(fd, VIDIOC_DQBUF, &buf);
```

进入：

```c
vb2_ioctl_dqbuf()
  -> vb2_dqbuf()
  -> vb2_core_dqbuf()
```

`vb2_core_dqbuf()` 里：

```c
ret = __vb2_get_done_vb(q, &vb, pb, nonblocking);
```

这一步从 `q->done_list` 里取一个 DONE buffer。

然后：

```c
call_void_vb_qop(vb, buf_finish, vb);
vb->prepared = 0;

list_del(&vb->queued_entry);
q->queued_count--;

__vb2_dqbuf(vb);
```

含义：

```text
调用驱动可选 .buf_finish()
prepared = 0
从 q->queued_list 删除
buffer 回到 DEQUEUED
应用层拿回 buffer
```

## 状态变化总表

```text
刚分配 / DQBUF 后
  state = DEQUEUED
  prepared = 0

QBUF 时
  __buf_prepare()
  prepared = 1
  synced = 1

加入 vb2 队列
  state = QUEUED
  挂入 q->queued_list

交给驱动
  __enqueue_in_driver()
  state = ACTIVE
  调用 .buf_queue()
  驱动挂入 g_queued_bufs

驱动填完
  vb2_buffer_done(DONE)
  synced = 0
  state = DONE
  挂入 q->done_list

应用层 DQBUF
  调用 .buf_finish()
  prepared = 0
  从 q->queued_list 删除
  state = DEQUEUED
```

## 你的虚拟摄像头里最重要的函数

```text
virtual_queue_setup()
  告诉 vb2 buffer 需要多大、几个 plane。

virtual_buf_queue()
  vb2 把 ACTIVE buffer 交给驱动。
  你把它挂到 g_queued_bufs。

virtual_start_streaming()
  开始定时器，模拟硬件出帧。

timer_function()
  从 g_queued_bufs 取 buffer，填图，vb2_buffer_done(DONE)。

virtual_stop_streaming()
  停定时器，并且要把 g_queued_bufs 里还没完成的 buffer 用 ERROR 还给 vb2。
```

## stop_streaming 必须还 buffer

停流时，驱动不能把 ACTIVE buffer 留在自己手里。

应该做类似：

```c
static void virtual_stop_streaming(struct vb2_queue *vq)
{
	struct virtual_frame_buf *buf;
	unsigned long flags;

	del_timer_sync(&timer);

	spin_lock_irqsave(&g_queued_bufs_lock, flags);
	while (!list_empty(&g_queued_bufs)) {
		buf = list_first_entry(&g_queued_bufs,
				       struct virtual_frame_buf,
				       list);
		list_del_init(&buf->list);
		spin_unlock_irqrestore(&g_queued_bufs_lock, flags);

		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);

		spin_lock_irqsave(&g_queued_bufs_lock, flags);
	}
	spin_unlock_irqrestore(&g_queued_bufs_lock, flags);
}
```

否则 vb2 会认为驱动还占着 buffer，停流时可能报警或者卡住。

## 最后记这条

```text
V4L2 负责把应用层 ioctl 接进来。
vb2 负责 buffer 生命周期和队列。
驱动负责在 .buf_queue() 后拿到 buffer，并在填完后调用 vb2_buffer_done()。
```

摄像头驱动的 buffer 主线就是：

```text
空 buffer 从应用层来，
驱动填成满 buffer，
满 buffer 再回应用层。
```
