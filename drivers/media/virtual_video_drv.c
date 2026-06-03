#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>
#include <linux/jiffies.h>
#include <linux/vmalloc.h>

#define VIRTUAL_WIDTH		800
#define VIRTUAL_HEIGHT		600
#define VIRTUAL_BPP		2
#define VIRTUAL_FRAME_SIZE	(VIRTUAL_WIDTH * VIRTUAL_HEIGHT * VIRTUAL_BPP)
#define VIRTUAL_FRAME_NUM	3

/*
 * 这个驱动是一个最小化的 V4L2 虚拟视频采集设备：
 *
 *   应用程序(v4l2-ctl/ffmpeg)
 *        |
 *        | open/ioctl/mmap/streamon/qbuf/dqbuf/streamoff
 *        v
 *   struct video_device + v4l2_file_operations
 *        |
 *        v
 *   videobuf2(vb2) 队列管理用户 buffer
 *        |
 *        v
 *   本驱动用 timer 周期性向空闲 buffer 填入 YUYV 测试帧
 *
 * 这里没有真实摄像头硬件，所以帧数据来自 g_test_frames[]。
 */

/* vb2_queue 是 videobuf2 的核心对象，负责管理 mmap/read/streaming buffer。 */
static struct vb2_queue g_vb_queue;

/*
 * 应用层 QBUF 后，vb2 会调用 virtual_buf_queue()。
 * 我们把这些等待填充的 buffer 挂到 g_queued_bufs 链表里，
 * 定时器每次取出一个 buffer，填入一帧，然后调用 vb2_buffer_done() 归还。
 */
static struct list_head g_queued_bufs;
static spinlock_t g_queued_bufs_lock;

/* 三帧不同颜色的 YUYV 测试图，循环输出。 */
static unsigned char *g_test_frames[VIRTUAL_FRAME_NUM];
static int g_frame_index = 0;

/*
 * 每个 vb2 buffer 对应一个本驱动私有的 buffer 结构。
 *
 * 注意：struct vb2_v4l2_buffer 必须放在第一个成员位置。
 * 这样 vb2 分配的内存可以安全地在 vb2_v4l2_buffer 和
 * virtual_frame_buf 之间通过 container_of() 转换。
 */
struct virtual_frame_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

/*
 * 统一填写本设备支持的视频格式。
 *
 * 本驱动只支持一种固定格式：
 *   - 分辨率：800x600
 *   - 像素格式：YUYV(YUYV422)
 *   - 每像素 2 字节，所以一帧大小为 width * height * 2
 *
 * G_FMT / TRY_FMT / S_FMT 都复用这个函数，保证用户空间看到的格式一致。
 */
static void virtual_fill_pix_format(struct v4l2_pix_format *pix)
{
	pix->width = VIRTUAL_WIDTH;
	pix->height = VIRTUAL_HEIGHT;
	pix->pixelformat = V4L2_PIX_FMT_YUYV;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = VIRTUAL_WIDTH * VIRTUAL_BPP;
	pix->sizeimage = VIRTUAL_FRAME_SIZE;
	pix->colorspace = V4L2_COLORSPACE_SRGB;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pix->quantization = V4L2_QUANTIZATION_DEFAULT;
	pix->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static int virtual_querycap(struct file *file, void *fh,
		struct v4l2_capability *cap)
{
	/*
	 * VIDIOC_QUERYCAP 用来告诉应用层：
	 *   这是什么驱动、什么设备、支持哪些能力。
	 *
	 * device_caps 必须和 struct video_device.device_caps 保持一致。
	 * V4L2_CAP_DEVICE_CAPS 表示应用层应查看 cap->device_caps 字段。
	 */
	strscpy(cap->driver, "hjy virtual", sizeof(cap->driver));
	strscpy(cap->card, "hjy no-card", sizeof(cap->card));
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int virtual_enum_fmt_vid_cap(struct file *file, void *fh,
				       struct v4l2_fmtdesc *f) 
{
	/*
	 * VIDIOC_ENUM_FMT 按 index 枚举支持的像素格式。
	 * 本驱动只有一种格式，所以 index=0 有效，其它 index 返回 -EINVAL。
	 */
	if (f->index > 0) {
		return -EINVAL;
	}

	f->flags = 0;
	strscpy(f->description, "YUYV 4:2:2", sizeof(f->description));
	f->pixelformat =  V4L2_PIX_FMT_YUYV; 

	return 0;
}

static int virtual_g_fmt_vid_cap(struct file *file, void *priv,
		struct v4l2_format *f)
{
	/* VIDIOC_G_FMT：返回当前格式。本驱动格式固定，直接填写即可。 */
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	virtual_fill_pix_format(&f->fmt.pix);
	return 0;
}

static int virtual_try_fmt_vid_cap(struct file *file, void *priv,
		struct v4l2_format *f)
{
	/*
	 * VIDIOC_TRY_FMT：应用层询问“如果我设置这个格式，驱动会接受成什么样”。
	 * 真实驱动通常会裁剪/对齐用户传入的宽高；这里格式固定，所以直接覆盖。
	 */
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	virtual_fill_pix_format(&f->fmt.pix);
	return 0;
}

static int virtual_s_fmt_vid_cap(struct file *file, void *priv,
		struct v4l2_format *f)
{
	/*
	 * VIDIOC_S_FMT：设置格式。
	 *
	 * vb2_is_busy() 用来保护正在使用的队列：
	 * 一旦已经 REQBUFS/STREAMON，用户就不能随便改格式，否则 buffer 大小
	 * 和实际帧大小可能不一致。
	 */
	if (vb2_is_busy(&g_vb_queue))
		return -EBUSY;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	virtual_fill_pix_format(&f->fmt.pix);
	return 0;
}


static int virtual_enum_framesizes(struct file *file, void *fh,
				      struct v4l2_frmsizeenum *fsize)
{
	/*
	 * VIDIOC_ENUM_FRAMESIZES：枚举某个像素格式支持的分辨率。
	 * 本驱动只有 YUYV 800x600 这一种离散尺寸。
	 */
	if (fsize->index > 0) {
		return -EINVAL;
	
	}
	if (fsize->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = VIRTUAL_WIDTH;
	fsize->discrete.height = VIRTUAL_HEIGHT;

	return 0;
}
static const struct v4l2_ioctl_ops virtual_video_ioctl_ops = {
	/*
	 * v4l2_ioctl_ops 是 video_ioctl2() 的分发表。
	 * 应用层调用 VIDIOC_xxx 时，V4L2 core 会根据这里的函数指针转到驱动。
	 */
	.vidioc_querycap          = virtual_querycap,

	.vidioc_enum_fmt_vid_cap  = virtual_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap     = virtual_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap   = virtual_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap     = virtual_s_fmt_vid_cap,
	.vidioc_enum_framesizes   = virtual_enum_framesizes,
	.vidioc_reqbufs           = vb2_ioctl_reqbufs,
	.vidioc_create_bufs       = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf       = vb2_ioctl_prepare_buf,
	.vidioc_querybuf          = vb2_ioctl_querybuf,
	.vidioc_qbuf              = vb2_ioctl_qbuf,
	.vidioc_dqbuf             = vb2_ioctl_dqbuf,

	.vidioc_streamon          = vb2_ioctl_streamon,
	.vidioc_streamoff         = vb2_ioctl_streamoff,
};


void timer_function (struct timer_list *timer)
{	
	struct virtual_frame_buf *buf;
	void* ptr;
	unsigned char *img = g_test_frames[g_frame_index++];
	unsigned long flags;

	/*
	 * timer 每触发一次，模拟硬件产生一帧图像。
	 * g_frame_index 在三张测试图之间循环，让抓到的视频能看到颜色变化。
	 */
	if (g_frame_index == 3) {
		g_frame_index = 0;
	}

	/*
	 * g_queued_bufs 会同时被 QBUF 路径、timer 路径、stop_streaming 路径访问，
	 * 所以必须加自旋锁。timer 在软中断上下文执行，使用 irqsave 更稳妥。
	 */
	spin_lock_irqsave(&g_queued_bufs_lock, flags);
	if (list_empty(&g_queued_bufs)) {
		spin_unlock_irqrestore(&g_queued_bufs_lock, flags);
		mod_timer(timer, jiffies + msecs_to_jiffies(300));
		return;
	}

	buf = list_entry(g_queued_bufs.next, struct virtual_frame_buf, list);
	
	/* 作用是从一个链表中删除指定的节点，并把它初始化成一个空的独立节点。 */
	list_del_init(&buf->list);
	
	spin_unlock_irqrestore(&g_queued_bufs_lock, flags);

	/*
	 * vb2_plane_vaddr() 获取当前 buffer 第 0 个 plane 的内核虚拟地址。
	 * 因为这里使用 vb2_vmalloc_memops，所以 buffer 在内核里可以直接 memcpy。
	 */
	ptr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	memcpy(ptr, img, 800 * 600 * 2);

	/*
	 * payload 表示这一帧实际有效数据大小。
	 * vb2_buffer_done(... DONE) 表示该 buffer 已经填好，DQBUF/read 可以取走。
	 */
	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, 800 * 600 * 2);
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

	/* 300ms 后继续产生下一帧，约 3.33 fps。 */
	mod_timer(timer, jiffies + msecs_to_jiffies(300));

}
struct timer_list timer;

/* 按 YUYV422 排列填充一整帧纯色图。每 4 字节表示两个像素：Y0 U Y1 V。 */
static void fill_yuyv_frame(unsigned char *buf, unsigned char y,
			    unsigned char u, unsigned char v)
{
	int i;

	for (i = 0; i < VIRTUAL_FRAME_SIZE; i += 4) {
		buf[i + 0] = y;
		buf[i + 1] = u;
		buf[i + 2] = y;
		buf[i + 3] = v;
	}
}

static int init_test_frames(void)
{
	int i;

	/* vmalloc 适合分配较大的连续虚拟地址内存；每帧 800*600*2 字节。 */
	for (i = 0; i < VIRTUAL_FRAME_NUM; i++) {
		g_test_frames[i] = vmalloc(VIRTUAL_FRAME_SIZE);
		if (!g_test_frames[i])
			goto err;
	}

	/*
	 * 这里填三种不同 YUV 值的纯色帧：
	 *   第 0 帧偏红
	 *   第 1 帧偏绿/浅色
	 *   第 2 帧偏蓝/亮色
	 * 这样抓视频时能确认帧在变化，而不是一直重复一帧。
	 */
	fill_yuyv_frame(g_test_frames[0], 76, 84, 255);
	fill_yuyv_frame(g_test_frames[1], 150, 44, 21);
	fill_yuyv_frame(g_test_frames[2], 29, 255, 107);

	g_frame_index = 0;
	return 0;

err:
	/* 如果中途分配失败，只释放已经成功分配的帧。 */
	while (--i >= 0) {
		vfree(g_test_frames[i]);
		g_test_frames[i] = NULL;
	}

	return -ENOMEM;
}

static void free_test_frames(void)
{
	int i;

	for (i = 0; i < VIRTUAL_FRAME_NUM; i++) {
		vfree(g_test_frames[i]);
		g_test_frames[i] = NULL;
	}
}

static const struct v4l2_file_operations virtual_video_fops = {
	/*
	 * 这是普通文件操作层：
	 *   open/release/read/poll/mmap/ioctl
	 *
	 * 大部分操作直接使用 V4L2/vb2 提供的通用 helper。
	 * video_ioctl2 会把 ioctl 分发到 virtual_video_ioctl_ops。
	 */
	.owner                    = THIS_MODULE,
	.open                     = v4l2_fh_open,
	.release                  = vb2_fop_release,
	.read                     = vb2_fop_read,
	.poll                     = vb2_fop_poll,
	.mmap                     = vb2_fop_mmap,
	.unlocked_ioctl           = video_ioctl2,
};

static struct video_device virtual_video = {
	/*
	 * video_device 描述一个 /dev/videoX 节点。
	 *
	 * device_caps 对注册阶段非常关键：
	 * Linux 5.10 的 __video_register_device() 会检查非 subdev 设备必须设置它。
	 */
	.name                     = "hjy_virtual_video",
	.release                  = video_device_release_empty,
	.fops                     = &virtual_video_fops,
	.ioctl_ops                = &virtual_video_ioctl_ops,
	.device_caps              = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE,
	.vfl_dir                  = VFL_DIR_RX,
};
/* vb2 队列锁：保护 vb2 队列状态。 */
static struct mutex vb_queue_lock;

/* V4L2 设备锁：V4L2 core 在 ioctl 等路径中可使用。 */
static struct mutex v4l2_lock;

/*
 * v4l2_device 是 V4L2 设备的上层容器。
 * 即使没有真实 platform_device/usb_device，也可以用 dev=NULL 注册，
 * 但这种情况下必须先填 v4l2_dev.name。
 */
static struct v4l2_device v4l2_dev;

static int virtual_queue_setup(struct vb2_queue *vq,
		unsigned int *nbuffers,
		unsigned int *nplanes, unsigned int sizes[], struct device *alloc_devs[])
{
	/*
	 * queue_setup 在 REQBUFS/CREATE_BUFS 阶段被 vb2 调用。
	 * 驱动需要告诉 vb2：
	 *   - 至少需要多少个 buffer
	 *   - 每个 buffer 有几个 plane
	 *   - 每个 plane 需要多大
	 *
	 * YUYV 是单平面格式，所以 nplanes=1。
	 */
	if (vq->num_buffers + *nbuffers < 8)
		*nbuffers = 8 - vq->num_buffers;
	*nplanes = 1;
	sizes[0] = PAGE_ALIGN(VIRTUAL_FRAME_SIZE);


	return 0;
}

static void virtual_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	struct virtual_frame_buf *buf =
			container_of(vbuf, struct virtual_frame_buf, vb);
	unsigned long flags;

	/*
	 * 应用层 QBUF 后，vb2 调用这里。
	 * 此时 buffer 已经属于驱动，驱动需要保存起来，等“硬件产生数据”后填充。
	 */
	spin_lock_irqsave(&g_queued_bufs_lock, flags);
	list_add_tail(&buf->list, &g_queued_bufs);
	spin_unlock_irqrestore(&g_queued_bufs_lock, flags);
}

static int virtual_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	/*
	 * STREAMON 后 vb2 调用 start_streaming。
	 * 真实摄像头驱动通常在这里启动 DMA/中断；本虚拟驱动启动 timer。
	 */
	timer_setup(&timer, timer_function, 0);
	timer.expires = jiffies + msecs_to_jiffies(30);
	add_timer(&timer);
	return 0;
}

static void virtual_stop_streaming(struct vb2_queue *vq)
{
	struct virtual_frame_buf *buf;
	struct virtual_frame_buf *tmp;
	unsigned long flags;

	/*
	 * STREAMOFF 或 close 时会走到这里。
	 * 必须先停止 timer，避免 timer 还在访问队列。
	 */
	del_timer_sync(&timer);

	/*
	 * vb2 有一个硬性要求：
	 * stop_streaming 返回前，驱动必须把所有还属于驱动的 buffer 归还给 vb2。
	 *
	 * 如果漏掉，就会看到：
	 *   driver bug: stop_streaming operation is leaving buf ... in active state
	 *
	 * 这里使用 VB2_BUF_STATE_ERROR，表示这些 buffer 没有拿到有效帧，
	 * 只是因为 streamoff/close 被迫结束。
	 */
	spin_lock_irqsave(&g_queued_bufs_lock, flags);
	list_for_each_entry_safe(buf, tmp, &g_queued_bufs, list) {
		list_del_init(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&g_queued_bufs_lock, flags);
}

static const struct vb2_ops virtual_video_vb2_ops = {
	/*
	 * vb2_ops 是 vb2 队列的驱动回调。
	 * 用户空间的 REQBUFS/QBUF/STREAMON/STREAMOFF 最终会触发这些函数。
	 */
	.queue_setup            = virtual_queue_setup,
	.buf_queue              = virtual_buf_queue,
	.start_streaming        = virtual_start_streaming,
	.stop_streaming         = virtual_stop_streaming,
	.wait_prepare           = vb2_ops_wait_prepare,
	.wait_finish            = vb2_ops_wait_finish,
};

static int __init virtual_video_drv_init(void)
{
    
	int ret = -1;

	ret = init_test_frames();
	if (ret) {
		printk("init_test_frames failed\n");
		return ret;
	}

	/* 初始化本驱动用到的锁和等待队列。 */
	mutex_init(&vb_queue_lock);
	mutex_init(&v4l2_lock);
	spin_lock_init(&g_queued_bufs_lock);
	INIT_LIST_HEAD(&g_queued_bufs);

	/*
	 * 初始化 videobuf2 队列。
	 *
	 * type 必须和 ioctl 中使用的 V4L2_BUF_TYPE_VIDEO_CAPTURE 一致。
	 * io_modes 决定用户空间可以用哪些方式取帧：
	 *   VB2_MMAP   : v4l2-ctl --stream-mmap
	 *   VB2_USERPTR: 用户传入自己的内存
	 *   VB2_DMABUF : 通过 dma-buf 共享
	 *   VB2_READ   : 支持 read() 方式读取
	 *
	 * mem_ops 选择 vb2_vmalloc_memops，说明 buffer 用 vmalloc 风格内存管理，
	 * 适合这种简单虚拟驱动学习使用。
	 */
	g_vb_queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	g_vb_queue.io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	g_vb_queue.buf_struct_size = sizeof(struct virtual_frame_buf);
	g_vb_queue.ops = &virtual_video_vb2_ops;
	g_vb_queue.mem_ops = &vb2_vmalloc_memops;
	g_vb_queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	g_vb_queue.lock = &vb_queue_lock;
	
	/* vb2_queue_init 会检查 vb2_queue 的关键字段是否完整。 */
	ret = vb2_queue_init(&g_vb_queue);
	if (ret) {
		printk( "Could not initialize vb2 queue\n");
		free_test_frames();
		return ret;
    }
    virtual_video.queue = &g_vb_queue;

	/*
	 * 因为 v4l2_device_register() 的 parent dev 参数传 NULL，
	 * 所以调用前必须手动填 v4l2_dev.name，否则内核会 WARN_ON。
	 */
	strscpy(v4l2_dev.name, "hjy_virtual", sizeof(v4l2_dev.name));
    ret = v4l2_device_register(NULL, &v4l2_dev) ;
    if (ret) {
        printk("v4l2_device_register\n");
		free_test_frames();
        return ret;
    }
    virtual_video.v4l2_dev = &v4l2_dev;
    virtual_video.lock = &v4l2_lock;

	/**
	*  video_register_device - 注册 video4linux 设备
	*
	* @vdev: 要注册的 struct video_device
	* @type: 要注册的设备类型，定义在 enum vfl_devnode_type 中
	* @nr:   期望的设备节点编号：
	*         (0 == /dev/video0, 1 == /dev/video1, ..., -1 == 第一个空闲编号)
	*
	* 内部实际调用的是 __video_register_device()，详细文档请参考那个函数。
	*
	* .. note::
	*      如果 video_register_device 失败，&struct video_device 结构体的
	*      release() 回调不会被调用，因此调用者需要自行负责释放所有数据。
	*      通常这意味着失败时你需要手动调用 video_device_release()。
	

	__must_check：编译器属性，表示返回值必须被检查，不能忽略。


	*/
    ret = video_register_device(&virtual_video, VFL_TYPE_VIDEO, -1);
    if (ret) {
        printk("video_register_device\n");
		v4l2_device_unregister(&v4l2_dev);
		free_test_frames();
        return ret;
    }
    return 0;

}

static void __exit virtual_video_drv_exit(void)
{
	/*
	 * 卸载顺序和注册顺序相反：
	 *   1. 先注销 /dev/videoX，阻止新的 open/ioctl
	 *   2. 再注销 v4l2_device
	 *   3. 最后释放测试帧内存
	 */
	video_unregister_device(&virtual_video);
	v4l2_device_unregister(&v4l2_dev);
	free_test_frames();
}

module_init(virtual_video_drv_init);
module_exit(virtual_video_drv_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hjy");
MODULE_DESCRIPTION("Virtual V4L2 video capture device");
