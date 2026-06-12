#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#define DEVICE_NAME		"virtual_video_dmabuf"
#define CHARDEV_NAME		"virtual_video_dma_ctrl"

#define VIRTUAL_WIDTH		800
#define VIRTUAL_HEIGHT		600
#define VIRTUAL_BPP		2
#define VIRTUAL_FRAME_SIZE	(VIRTUAL_WIDTH * VIRTUAL_HEIGHT * VIRTUAL_BPP)
#define VIRTUAL_FRAME_NUM	3
#define VIRTUAL_FRAME_INTERVAL_MS 300

struct virtual_test_frame {
	void *cpu_addr;
	dma_addr_t dma_addr;
};

struct virtual_frame_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct virtual_video_dev {
	struct device *dev;

	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue vb_queue;
	struct mutex vb_queue_lock;
	struct mutex v4l2_lock;

	struct miscdevice miscdev;
	struct mutex misc_lock;

	struct list_head queued_bufs;
	spinlock_t queued_bufs_lock;

	struct timer_list timer;
	bool streaming;

	struct dma_chan *dma_chan;
	bool dma_busy;
	struct virtual_frame_buf *active_buf;

	struct virtual_test_frame test_frames[VIRTUAL_FRAME_NUM];
	int frame_index;
};

struct virtual_dma_xfer {
	struct virtual_video_dev *vvd;
	struct virtual_frame_buf *buf;
};

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
	strscpy(cap->driver, DEVICE_NAME, sizeof(cap->driver));
	strscpy(cap->card, "hjy virtual dma video", sizeof(cap->card));
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			   V4L2_CAP_READWRITE;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int virtual_enum_fmt_vid_cap(struct file *file, void *fh,
				    struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->flags = 0;
	strscpy(f->description, "YUYV 4:2:2", sizeof(f->description));
	f->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static int virtual_g_fmt_vid_cap(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	virtual_fill_pix_format(&f->fmt.pix);
	return 0;
}

static int virtual_try_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	virtual_fill_pix_format(&f->fmt.pix);
	return 0;
}

static int virtual_s_fmt_vid_cap(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct virtual_video_dev *vvd = video_drvdata(file);

	if (vb2_is_busy(&vvd->vb_queue))
		return -EBUSY;
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	virtual_fill_pix_format(&f->fmt.pix);
	return 0;
}

static int virtual_enum_framesizes(struct file *file, void *fh,
				   struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index > 0)
		return -EINVAL;
	if (fsize->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = VIRTUAL_WIDTH;
	fsize->discrete.height = VIRTUAL_HEIGHT;
	return 0;
}

static const struct v4l2_ioctl_ops virtual_video_ioctl_ops = {
	.vidioc_querycap	= virtual_querycap,
	.vidioc_enum_fmt_vid_cap = virtual_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap	= virtual_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap	= virtual_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap	= virtual_s_fmt_vid_cap,
	.vidioc_enum_framesizes	= virtual_enum_framesizes,
	.vidioc_reqbufs		= vb2_ioctl_reqbufs,
	.vidioc_create_bufs	= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf	= vb2_ioctl_prepare_buf,
	.vidioc_querybuf	= vb2_ioctl_querybuf,
	.vidioc_qbuf		= vb2_ioctl_qbuf,
	.vidioc_dqbuf		= vb2_ioctl_dqbuf,
	.vidioc_streamon	= vb2_ioctl_streamon,
	.vidioc_streamoff	= vb2_ioctl_streamoff,
};

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

static int virtual_init_test_frames(struct virtual_video_dev *vvd)
{
	int i;

	// for (i = 0; i < VIRTUAL_FRAME_NUM; i++) {
	// 	vvd->test_frames[i].cpu_addr =
	// 		dma_alloc_coherent(vvd->dev, VIRTUAL_FRAME_SIZE,
	// 				   &vvd->test_frames[i].dma_addr,
	// 				   GFP_KERNEL);
	// 	if (!vvd->test_frames[i].cpu_addr)
	// 		goto err;
	// }

	for (i = 0; i < VIRTUAL_FRAME_NUM; ++i) {
		vvd->test_frames[i].cpu_addr = kmalloc(VIRTUAL_FRAME_SIZE, GFP_KERNEL);
		if (vvd->test_frames[i].cpu_addr == NULL) {
			 dev_err(vvd->dev, "dma_map_single failed for frame %d\n", i);
			 goto err_unmap;
		}
		if (i == 0) {
			fill_yuyv_frame(vvd->test_frames[i].cpu_addr, 76, 84, 255);
		} else if (i == 1) {
			fill_yuyv_frame(vvd->test_frames[i].cpu_addr, 150, 44, 21);
		} else if (i == 2) {
			fill_yuyv_frame(vvd->test_frames[i].cpu_addr, 29, 255, 107);

		}
		vvd->test_frames[i].dma_addr =dma_map_single(vvd->dev, vvd->test_frames[i].cpu_addr, VIRTUAL_FRAME_SIZE, DMA_TO_DEVICE);
		if (dma_mapping_error(vvd->dev, vvd->test_frames[i].dma_addr)) {
		dev_err(vvd->dev, "dma_map_single failed for frame %d\n", i);
			kfree(vvd->test_frames[i].cpu_addr);
			vvd->test_frames[i].cpu_addr = NULL;
			// 需要回滚之前已映射成功的帧
		goto err_unmap;
}
	}

	
	for (i = 0; i < VIRTUAL_FRAME_NUM; i++)
		dev_info(vvd->dev, "test frame[%d]: cpu=%p streaming dma=%pad\n",
			 i, vvd->test_frames[i].cpu_addr,
			 &vvd->test_frames[i].dma_addr);

	vvd->frame_index = 0;
	return 0;

err_unmap:
	while (--i >= 0) {
		// dma_free_coherent(vvd->dev, VIRTUAL_FRAME_SIZE,
		// 		  vvd->test_frames[i].cpu_addr,
		// 		  vvd->test_frames[i].dma_addr);
		
		dma_unmap_single(vvd->dev, vvd->test_frames[i].dma_addr, VIRTUAL_FRAME_SIZE, DMA_TO_DEVICE);
		kfree(vvd->test_frames[i].cpu_addr);
		vvd->test_frames[i].cpu_addr = NULL;
		vvd->test_frames[i].dma_addr = 0;
	}
	return -ENOMEM;
}

static void virtual_free_test_frames(struct virtual_video_dev *vvd)
{
	int i;

	for (i = 0; i < VIRTUAL_FRAME_NUM; i++) {
		if (!vvd->test_frames[i].cpu_addr)
			continue;
		dma_unmap_single(vvd->dev, vvd->test_frames[i].dma_addr,
				  VIRTUAL_FRAME_SIZE, DMA_TO_DEVICE);
		kfree(vvd->test_frames[i].cpu_addr);
		vvd->test_frames[i].cpu_addr = NULL;
		vvd->test_frames[i].dma_addr = 0;
	}
}

static struct dma_chan *virtual_request_dma_chan(struct virtual_video_dev *vvd)
{
	dma_cap_mask_t mask;
	struct dma_chan *chan;

	// chan = dma_request_chan(vvd->dev, "memcpy");
	// if (!IS_ERR(chan))
	// 	return chan;

	// dev_info(vvd->dev, "dma_request_chan(\"memcpy\") failed: %ld, fallback to any DMA_MEMCPY channel\n",
	// 	 PTR_ERR(chan));

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	chan = dma_request_channel(mask, NULL, NULL);
	if (!chan)
		return ERR_PTR(-ENODEV);
	dev_info(vvd->dev, "virtual_request_dma_chan successful\n");
	return chan;
}

static void virtual_dma_complete(void *param)
{
	struct virtual_dma_xfer *xfer = param;
	struct virtual_video_dev *vvd = xfer->vvd;
	struct virtual_frame_buf *buf = xfer->buf;
	unsigned long flags;

	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, VIRTUAL_FRAME_SIZE);
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

	spin_lock_irqsave(&vvd->queued_bufs_lock, flags);
	vvd->active_buf = NULL;
	vvd->dma_busy = false;
	spin_unlock_irqrestore(&vvd->queued_bufs_lock, flags);

	kfree(xfer);

	if (READ_ONCE(vvd->streaming))
		mod_timer(&vvd->timer,
			  jiffies + msecs_to_jiffies(VIRTUAL_FRAME_INTERVAL_MS));
}

static void virtual_timer_function(struct timer_list *timer)
{
	struct virtual_video_dev *vvd = from_timer(vvd, timer, timer);
	struct virtual_dma_xfer *xfer;
	struct virtual_frame_buf *buf;
	struct dma_async_tx_descriptor *desc;
	dma_addr_t src_dma_addr;
	dma_addr_t dst_dma_addr;
	dma_cookie_t cookie;
	unsigned long flags;

	if (!READ_ONCE(vvd->streaming))
		return;

	spin_lock_irqsave(&vvd->queued_bufs_lock, flags);
	if (vvd->dma_busy || list_empty(&vvd->queued_bufs)) {
		spin_unlock_irqrestore(&vvd->queued_bufs_lock, flags);
		mod_timer(&vvd->timer,
			  jiffies + msecs_to_jiffies(VIRTUAL_FRAME_INTERVAL_MS));
		return;
	}

	buf = list_first_entry(&vvd->queued_bufs, struct virtual_frame_buf, list);
	list_del_init(&buf->list);
	vvd->active_buf = buf;
	vvd->dma_busy = true;
	spin_unlock_irqrestore(&vvd->queued_bufs_lock, flags);

	src_dma_addr = vvd->test_frames[vvd->frame_index++].dma_addr;
	if (vvd->frame_index >= VIRTUAL_FRAME_NUM)
		vvd->frame_index = 0;

	dst_dma_addr = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
	if (!dst_dma_addr) {
		dev_err(vvd->dev, "failed to get vb2 dma address\n");
		goto err_done;
	}

	xfer = kzalloc(sizeof(*xfer), GFP_ATOMIC);
	if (!xfer)
		goto err_done;
	xfer->vvd = vvd;
	xfer->buf = buf;

	desc = dmaengine_prep_dma_memcpy(vvd->dma_chan, dst_dma_addr,
					 src_dma_addr, VIRTUAL_FRAME_SIZE,
					 DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc) {
		dev_err(vvd->dev, "dmaengine_prep_dma_memcpy failed\n");
		kfree(xfer);
		goto err_done;
	}

	desc->callback = virtual_dma_complete;
	desc->callback_param = xfer;

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie)) {
		dev_err(vvd->dev, "dmaengine_submit failed: %d\n", cookie);
		kfree(xfer);
		goto err_done;
	}

	dma_async_issue_pending(vvd->dma_chan);
	return;

err_done:
	spin_lock_irqsave(&vvd->queued_bufs_lock, flags);
	vvd->active_buf = NULL;
	vvd->dma_busy = false;
	spin_unlock_irqrestore(&vvd->queued_bufs_lock, flags);

	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	if (READ_ONCE(vvd->streaming))
		mod_timer(&vvd->timer,
			  jiffies + msecs_to_jiffies(VIRTUAL_FRAME_INTERVAL_MS));
}

static int virtual_queue_setup(struct vb2_queue *vq,
			       unsigned int *nbuffers,
			       unsigned int *nplanes,
			       unsigned int sizes[],
			       struct device *alloc_devs[])
{
	if (vq->num_buffers + *nbuffers < 8)
		*nbuffers = 8 - vq->num_buffers;

	if (*nplanes) {
		if (*nplanes != 1 || sizes[0] < VIRTUAL_FRAME_SIZE)
			return -EINVAL;
		return 0;
	}

	*nplanes = 1;
	sizes[0] = PAGE_ALIGN(VIRTUAL_FRAME_SIZE);
	return 0;
}

static void virtual_buf_queue(struct vb2_buffer *vb)
{
	struct virtual_video_dev *vvd = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct virtual_frame_buf *buf =
		container_of(vbuf, struct virtual_frame_buf, vb);
	unsigned long flags;

	spin_lock_irqsave(&vvd->queued_bufs_lock, flags);
	list_add_tail(&buf->list, &vvd->queued_bufs);
	spin_unlock_irqrestore(&vvd->queued_bufs_lock, flags);
}

static int virtual_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct virtual_video_dev *vvd = vb2_get_drv_priv(vq);

	WRITE_ONCE(vvd->streaming, true);
	mod_timer(&vvd->timer, jiffies + msecs_to_jiffies(30));
	return 0;
}

static void virtual_return_queued_buffers(struct virtual_video_dev *vvd)
{
	struct virtual_frame_buf *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&vvd->queued_bufs_lock, flags);
	list_for_each_entry_safe(buf, tmp, &vvd->queued_bufs, list) {
		list_del_init(&buf->list);
		spin_unlock_irqrestore(&vvd->queued_bufs_lock, flags);

		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);

		spin_lock_irqsave(&vvd->queued_bufs_lock, flags);
	}
	spin_unlock_irqrestore(&vvd->queued_bufs_lock, flags);
}

static void virtual_stop_streaming(struct vb2_queue *vq)
{
	struct virtual_video_dev *vvd = vb2_get_drv_priv(vq);
	struct virtual_frame_buf *active;
	unsigned long flags;

	WRITE_ONCE(vvd->streaming, false);
	del_timer_sync(&vvd->timer);

	if (vvd->dma_chan)
		dmaengine_terminate_sync(vvd->dma_chan);

	spin_lock_irqsave(&vvd->queued_bufs_lock, flags);
	active = vvd->active_buf;
	vvd->active_buf = NULL;
	vvd->dma_busy = false;
	spin_unlock_irqrestore(&vvd->queued_bufs_lock, flags);

	if (active)
		vb2_buffer_done(&active->vb.vb2_buf, VB2_BUF_STATE_ERROR);

	virtual_return_queued_buffers(vvd);
}

static const struct vb2_ops virtual_video_vb2_ops = {
	.queue_setup	 = virtual_queue_setup,
	.buf_queue	 = virtual_buf_queue,
	.start_streaming = virtual_start_streaming,
	.stop_streaming	 = virtual_stop_streaming,
	.wait_prepare	 = vb2_ops_wait_prepare,
	.wait_finish	 = vb2_ops_wait_finish,
};

static const struct v4l2_file_operations virtual_video_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.read		= vb2_fop_read,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
	.unlocked_ioctl	= video_ioctl2,
};

static int virtual_misc_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct virtual_video_dev *vvd;

	vvd = container_of(miscdev, struct virtual_video_dev, miscdev);
	filp->private_data = vvd;
	return 0;
}

static ssize_t virtual_misc_read(struct file *filp, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct virtual_video_dev *vvd = filp->private_data;
	int i;

	mutex_lock(&vvd->misc_lock);
	dev_info(vvd->dev, "video node=/dev/%s dma channel=%s streaming=%d busy=%d\n",
		 video_device_node_name(&vvd->vdev),
		 dma_chan_name(vvd->dma_chan), vvd->streaming, vvd->dma_busy);
	for (i = 0; i < VIRTUAL_FRAME_NUM; i++)
		dev_info(vvd->dev, "frame[%d]: cpu=%p dma=%pad\n",
			 i, vvd->test_frames[i].cpu_addr,
			 &vvd->test_frames[i].dma_addr);
	mutex_unlock(&vvd->misc_lock);

	return 0;
}

static const struct file_operations virtual_misc_fops = {
	.owner = THIS_MODULE,
	.open = virtual_misc_open,
	.read = virtual_misc_read,
};

static int virtual_video_probe(struct platform_device *pdev)
{
	struct virtual_video_dev *vvd;
	int ret;

	vvd = devm_kzalloc(&pdev->dev, sizeof(*vvd), GFP_KERNEL);
	if (!vvd)
		return -ENOMEM;

	vvd->dev = &pdev->dev;
	mutex_init(&vvd->vb_queue_lock);
	mutex_init(&vvd->v4l2_lock);
	mutex_init(&vvd->misc_lock);
	spin_lock_init(&vvd->queued_bufs_lock);
	INIT_LIST_HEAD(&vvd->queued_bufs);
	timer_setup(&vvd->timer, virtual_timer_function, 0);

	vvd->dma_chan = virtual_request_dma_chan(vvd);
	if (IS_ERR(vvd->dma_chan)) {
		ret = PTR_ERR(vvd->dma_chan);
		dev_err(vvd->dev, "request DMA channel failed: %d\n", ret);
		return ret;
	}

	ret = virtual_init_test_frames(vvd);
	if (ret)
		goto err_release_dma;

	vvd->vb_queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vvd->vb_queue.io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ;
	vvd->vb_queue.buf_struct_size = sizeof(struct virtual_frame_buf);
	vvd->vb_queue.ops = &virtual_video_vb2_ops;
	vvd->vb_queue.mem_ops = &vb2_dma_contig_memops;
	vvd->vb_queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vvd->vb_queue.lock = &vvd->vb_queue_lock;
	vvd->vb_queue.dev = vvd->dev;
	vvd->vb_queue.drv_priv = vvd;

	ret = vb2_queue_init(&vvd->vb_queue);
	if (ret) {
		dev_err(vvd->dev, "vb2_queue_init failed: %d\n", ret);
		goto err_free_frames;
	}

	strscpy(vvd->v4l2_dev.name, DEVICE_NAME, sizeof(vvd->v4l2_dev.name));
	ret = v4l2_device_register(vvd->dev, &vvd->v4l2_dev);
	if (ret) {
		dev_err(vvd->dev, "v4l2_device_register failed: %d\n", ret);
		goto err_free_frames;
	}

	strscpy(vvd->vdev.name, DEVICE_NAME, sizeof(vvd->vdev.name));
	vvd->vdev.release = video_device_release_empty;
	vvd->vdev.fops = &virtual_video_fops;
	vvd->vdev.ioctl_ops = &virtual_video_ioctl_ops;
	vvd->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
				V4L2_CAP_READWRITE;
	vvd->vdev.vfl_dir = VFL_DIR_RX;
	vvd->vdev.queue = &vvd->vb_queue;
	vvd->vdev.v4l2_dev = &vvd->v4l2_dev;
	vvd->vdev.lock = &vvd->v4l2_lock;
	video_set_drvdata(&vvd->vdev, vvd);

	ret = video_register_device(&vvd->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(vvd->dev, "video_register_device failed: %d\n", ret);
		goto err_unreg_v4l2;
	}

	vvd->miscdev.minor = MISC_DYNAMIC_MINOR;
	vvd->miscdev.name = CHARDEV_NAME;
	vvd->miscdev.fops = &virtual_misc_fops;

	ret = misc_register(&vvd->miscdev);
	if (ret) {
		dev_err(vvd->dev, "misc_register failed: %d\n", ret);
		goto err_unreg_video;
	}

	platform_set_drvdata(pdev, vvd);
	dev_info(vvd->dev, "probe success: video=%s chardev=/dev/%s dma=%s\n",
		 video_device_node_name(&vvd->vdev), CHARDEV_NAME,
		 dma_chan_name(vvd->dma_chan));
	return 0;

err_unreg_video:
	video_unregister_device(&vvd->vdev);
err_unreg_v4l2:
	v4l2_device_unregister(&vvd->v4l2_dev);
err_free_frames:
	virtual_free_test_frames(vvd);
err_release_dma:
	dma_release_channel(vvd->dma_chan);
	return ret;
}

static int virtual_video_remove(struct platform_device *pdev)
{
	struct virtual_video_dev *vvd = platform_get_drvdata(pdev);

	misc_deregister(&vvd->miscdev);
	video_unregister_device(&vvd->vdev);
	v4l2_device_unregister(&vvd->v4l2_dev);

	WRITE_ONCE(vvd->streaming, false);
	del_timer_sync(&vvd->timer);
	if (vvd->dma_chan) {
		dmaengine_terminate_sync(vvd->dma_chan);
		dma_release_channel(vvd->dma_chan);
	}

	virtual_free_test_frames(vvd);
	dev_info(&pdev->dev, "remove success\n");
	return 0;
}

static const struct of_device_id virtual_video_of_match[] = {
	{ .compatible = "hjy,virtual-video-dmabuf" },
	{ .compatible = "hjy,dma_test" },
	{ }
};
MODULE_DEVICE_TABLE(of, virtual_video_of_match);

static struct platform_driver virtual_video_driver = {
	.probe = virtual_video_probe,
	.remove = virtual_video_remove,
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = virtual_video_of_match,
	},
};

module_platform_driver(virtual_video_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hjy");
MODULE_DESCRIPTION("Virtual V4L2 DMA video capture device with misc chardev");
