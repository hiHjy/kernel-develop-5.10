#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

/*
 * 简易 dmabuf exporter 教学模板。
 *
 * 第一版目标：
 *   1. platform_driver 通过设备树匹配。
 *   2. probe 里注册 misc 字符设备 /dev/simple_dmabuf_exporter。
 *   3. 用户态 ioctl EXPORT，驱动申请一块 coherent DMA buffer。
 *   4. 驱动用 dma_buf_export() 包装这块 buffer。
 *   5. 驱动用 dma_buf_fd() 把 struct dma_buf 变成用户态 fd。
 *   6. 用户态可以 mmap 这个 dmabuf fd，然后直接读写 buffer。
 *
 * 这版故意不碰 fence / reservation / sync_file。
 *
 * fence 是 dmabuf 生态里的同步机制，常用于 GPU/DRM/显示/多硬件流水线。
 * 你现在先把 exporter、fd、mmap、release 生命周期跑通，后面真正做
 * 多硬件异步同步时再学 fence，会清楚很多。
 */

#define DEVICE_NAME "simple_dmabuf_exporter"
#define DUMP_SIZE 100

#define SIMPLE_DMABUF_IOC_MAGIC 'b'
#define SIMPLE_DMABUF_IOC_EXPORT \
	_IOWR(SIMPLE_DMABUF_IOC_MAGIC, 1, struct simple_dmabuf_export_arg)
#define SIMPLE_DMABUF_IOC_DUMP _IO(SIMPLE_DMABUF_IOC_MAGIC, 2)

/*
 * 用户态 EXPORT 参数。
 *
 * size:
 *   用户传入，表示想导出多大的 buffer。
 *
 * fd:
 *   内核返回，表示导出的 dmabuf fd。
 *
 * 用户态典型流程：
 *
 *   struct simple_dmabuf_export_arg arg = {
 *       .size = 4096,
 *   };
 *
 *   ioctl(dev_fd, SIMPLE_DMABUF_IOC_EXPORT, &arg);
 *
 *   dmabuf_fd = arg.fd;
 *   ptr = mmap(NULL, arg.size, PROT_READ | PROT_WRITE,
 *              MAP_SHARED, dmabuf_fd, 0);
 */
struct simple_dmabuf_export_arg {
	__u32 size;
	__s32 fd;
};

struct simple_dmabuf_dev {
	struct device *dev;
	struct miscdevice miscdev;
	struct mutex lock;

	/*
	 * 教学调试用：
	 *
	 * 保存最近一次导出的 buffer，方便 ioctl DUMP 打印。
	 * 真实项目里要用更严格的引用计数/列表管理多个 buffer。
	 */
	struct dma_buf *last_dmabuf;
};

struct simple_dmabuf_buffer {
	struct device *dev;
	struct mutex lock;

	void *cpu_addr;
	dma_addr_t dma_addr;
	size_t size;

	/*
	 * release 只应该执行一次。
	 * buffer 真正生命周期由 dmabuf fd 引用计数决定。
	 */
	bool released;
};

static const struct of_device_id simple_dmabuf_of_match[] = {
	{ .compatible = "hjy,simple-dmabuf-exporter" },
	{ }
};
MODULE_DEVICE_TABLE(of, simple_dmabuf_of_match);

static void simple_dmabuf_dump_buffer(struct simple_dmabuf_buffer *buf,
				      const char *reason)
{
	size_t dump_size;

	if (!buf || !buf->cpu_addr)
		return;

	dump_size = min_t(size_t, buf->size, DUMP_SIZE);

	dev_info(buf->dev, "%s: dump first %zu bytes, cpu=%p dma=%pad size=%zu\n",
		 reason, dump_size, buf->cpu_addr, &buf->dma_addr, buf->size);

	print_hex_dump(KERN_INFO, "simple_dmabuf: ",
		       DUMP_PREFIX_OFFSET, 16, 1,
		       buf->cpu_addr, dump_size, true);
}

/*
 * dma_buf_ops.attach
 *
 * importer 调用 dma_buf_attach(dmabuf, importer_dev) 时进入这里。
 *
 * 第一版只做日志和放行。真实 exporter 可以在这里检查：
 *   1. importer_dev 的 DMA mask 是否能访问这块内存。
 *   2. buffer 是否在 importer 可访问的位置。
 *   3. 是否需要迁移 backing storage。
 */
static int simple_dmabuf_attach(struct dma_buf *dmabuf,
				struct dma_buf_attachment *attach)
{
	struct simple_dmabuf_buffer *buf = dmabuf->priv;

	dev_info(buf->dev, "attach: importer dev=%s\n",
		 dev_name(attach->dev));

	return 0;
}

/*
 * dma_buf_ops.detach
 *
 * importer 不再使用这块 dmabuf 时进入这里。
 */
static void simple_dmabuf_detach(struct dma_buf *dmabuf,
				 struct dma_buf_attachment *attach)
{
	struct simple_dmabuf_buffer *buf = dmabuf->priv;

	dev_info(buf->dev, "detach: importer dev=%s\n",
		 dev_name(attach->dev));
}

/*
 * dma_buf_ops.map_dma_buf
 *
 * importer 调用 dma_buf_map_attachment() 时进入这里。
 *
 * 这里要返回一个已经映射到 importer 设备地址空间的 sg_table。
 * 为了教学简单，我们把 coherent buffer 看成一段连续内存，构造 1 个
 * scatterlist entry。
 *
 * 注意：
 *   dma_mmap_coherent() 是给用户态 mmap 用的。
 *   map_dma_buf() 是给另一个设备/importer DMA 访问用的。
 */
static struct sg_table *
simple_dmabuf_map_dma_buf(struct dma_buf_attachment *attach,
			  enum dma_data_direction direction)
{
	struct simple_dmabuf_buffer *buf = attach->dmabuf->priv;
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (ret)
		goto err_free_sgt;

	sg_dma_address(sgt->sgl) = buf->dma_addr;
	sg_dma_len(sgt->sgl) = buf->size;
	sg_set_page(sgt->sgl, virt_to_page(buf->cpu_addr), buf->size, 0);

	ret = dma_map_sg_attrs(attach->dev, sgt->sgl, sgt->nents, direction, 0);
	if (!ret) {
		ret = -ENOMEM;
		goto err_free_table;
	}

	dev_info(buf->dev, "map_dma_buf: importer dev=%s nents=%d dir=%d\n",
		 dev_name(attach->dev), ret, direction);

	return sgt;

err_free_table:
	sg_free_table(sgt);
err_free_sgt:
	kfree(sgt);
	return ERR_PTR(ret);
}

/*
 * dma_buf_ops.unmap_dma_buf
 *
 * 和 map_dma_buf 成对出现。
 */
static void simple_dmabuf_unmap_dma_buf(struct dma_buf_attachment *attach,
					struct sg_table *sgt,
					enum dma_data_direction direction)
{
	struct simple_dmabuf_buffer *buf = attach->dmabuf->priv;

	dev_info(buf->dev, "unmap_dma_buf: importer dev=%s dir=%d\n",
		 dev_name(attach->dev), direction);

	dma_unmap_sg_attrs(attach->dev, sgt->sgl, sgt->nents, direction, 0);
	sg_free_table(sgt);
	kfree(sgt);
}

/*
 * dma_buf_ops.mmap
 *
 * 用户态对 dmabuf fd 调 mmap() 时进入这里。
 *
 * 这和你前面字符设备 mmap 很像，只不过 fd 不是 misc 设备 fd，
 * 而是 dma_buf_fd() 返回的 dmabuf fd。
 */
static int simple_dmabuf_mmap(struct dma_buf *dmabuf,
			      struct vm_area_struct *vma)
{
	struct simple_dmabuf_buffer *buf = dmabuf->priv;
	unsigned long req_size = vma->vm_end - vma->vm_start;

	if (req_size > buf->size)
		return -EINVAL;

	/*
	 * 用户态 mmap dmabuf fd 时，正常 offset 是 0。
	 * 这里直接把 coherent buffer 映射给用户态。
	 */
	return dma_mmap_coherent(buf->dev, vma, buf->cpu_addr,
				 buf->dma_addr, buf->size);
}

/*
 * dma_buf_ops.begin_cpu_access / end_cpu_access
 *
 * coherent buffer 第一版不需要手动 cache sync。
 * 但我们仍然放两个回调，方便你观察用户态 DMA_BUF_IOCTL_SYNC 或
 * 其他内核用户触发 CPU access 边界时会走到这里。
 */
static int simple_dmabuf_begin_cpu_access(struct dma_buf *dmabuf,
					  enum dma_data_direction direction)
{
	struct simple_dmabuf_buffer *buf = dmabuf->priv;

	dev_info(buf->dev, "begin_cpu_access: dir=%d\n", direction);
	return 0;
}

static int simple_dmabuf_end_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction direction)
{
	struct simple_dmabuf_buffer *buf = dmabuf->priv;

	dev_info(buf->dev, "end_cpu_access: dir=%d\n", direction);
	return 0;
}

/*
 * dma_buf_ops.release
 *
 * 最后一个 dmabuf 引用释放时进入这里。
 *
 * 用户态 close(dmabuf_fd) 不一定立刻调用 release：
 *   如果还有别的进程/驱动/importer 持有引用，release 会延后。
 *
 * 这就是 dmabuf 生命周期和 misc 字符设备 fd 生命周期的区别。
 */
static void simple_dmabuf_release(struct dma_buf *dmabuf)
{
	struct simple_dmabuf_buffer *buf = dmabuf->priv;

	mutex_lock(&buf->lock);
	if (buf->released) {
		mutex_unlock(&buf->lock);
		return;
	}
	buf->released = true;
	mutex_unlock(&buf->lock);

	simple_dmabuf_dump_buffer(buf, "release");

	dma_free_coherent(buf->dev, buf->size, buf->cpu_addr, buf->dma_addr);
	kfree(buf);
}

static const struct dma_buf_ops simple_dmabuf_ops = {
	.attach = simple_dmabuf_attach,
	.detach = simple_dmabuf_detach,
	.map_dma_buf = simple_dmabuf_map_dma_buf,
	.unmap_dma_buf = simple_dmabuf_unmap_dma_buf,
	.mmap = simple_dmabuf_mmap,
	.begin_cpu_access = simple_dmabuf_begin_cpu_access,
	.end_cpu_access = simple_dmabuf_end_cpu_access,
	.release = simple_dmabuf_release,
};

static int simple_dmabuf_export_one(struct simple_dmabuf_dev *sdev,
				    size_t size)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct simple_dmabuf_buffer *buf;
	struct dma_buf *dmabuf;
	int fd;

	if (!size)
		return -EINVAL;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf->dev = sdev->dev;
	buf->size = PAGE_ALIGN(size);
	mutex_init(&buf->lock);

	buf->cpu_addr = dma_alloc_coherent(buf->dev, buf->size,
					   &buf->dma_addr, GFP_KERNEL);
	if (!buf->cpu_addr) {
		kfree(buf);
		return -ENOMEM;
	}

	memset(buf->cpu_addr, 0x00, buf->size);

	exp_info.ops = &simple_dmabuf_ops;
	exp_info.size = buf->size;
	exp_info.flags = O_RDWR;
	exp_info.priv = buf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		dma_free_coherent(buf->dev, buf->size, buf->cpu_addr,
				  buf->dma_addr);
		kfree(buf);
		return PTR_ERR(dmabuf);
	}

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0) {
		/*
		 * dma_buf_fd() 失败时，要释放 dma_buf 引用。
		 * release 回调会负责释放 backing buffer。
		 */
		dma_buf_put(dmabuf);
		return fd;
	}

	mutex_lock(&sdev->lock);
	if (sdev->last_dmabuf)
		dma_buf_put(sdev->last_dmabuf);
	get_dma_buf(dmabuf);
	sdev->last_dmabuf = dmabuf;
	mutex_unlock(&sdev->lock);

	dev_info(sdev->dev,
		 "export dmabuf: size=%zu fd=%d cpu=%p dma=%pad\n",
		 buf->size, fd, buf->cpu_addr, &buf->dma_addr);

	return fd;
}

static int simple_dmabuf_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct simple_dmabuf_dev *sdev;

	sdev = container_of(miscdev, struct simple_dmabuf_dev, miscdev);
	filp->private_data = sdev;

	return 0;
}

static int simple_dmabuf_release_file(struct inode *inode, struct file *filp)
{
	return 0;
}

static long simple_dmabuf_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	struct simple_dmabuf_dev *sdev = filp->private_data;
	struct simple_dmabuf_export_arg export_arg;
	struct simple_dmabuf_buffer *buf;
	struct dma_buf *dmabuf;
	long ret = 0;
	int fd;

	if (_IOC_TYPE(cmd) != SIMPLE_DMABUF_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case SIMPLE_DMABUF_IOC_EXPORT:
		if (copy_from_user(&export_arg, (void __user *)arg,
				   sizeof(export_arg)))
			return -EFAULT;

		fd = simple_dmabuf_export_one(sdev, export_arg.size);
		if (fd < 0)
			return fd;

		export_arg.size = PAGE_ALIGN(export_arg.size);
		export_arg.fd = fd;

		if (copy_to_user((void __user *)arg, &export_arg,
				 sizeof(export_arg))) {
			/*
			 * 如果 fd 已经分配给当前进程，但 copy_to_user 失败，
			 * 严格来说还应该关闭这个 fd。教学模板先返回错误，
			 * 后面你可以自己补 close_fd() 这一层。
			 */
			return -EFAULT;
		}
		break;

	case SIMPLE_DMABUF_IOC_DUMP:
		mutex_lock(&sdev->lock);
		dmabuf = sdev->last_dmabuf;
		if (dmabuf)
			get_dma_buf(dmabuf);
		mutex_unlock(&sdev->lock);

		if (!dmabuf)
			return -ENOENT;

		buf = dmabuf->priv;
		simple_dmabuf_dump_buffer(buf, "ioctl dump");
		dma_buf_put(dmabuf);
		break;

	default:
		ret = -ENOTTY;
		break;
	}

	return ret;
}

static const struct file_operations simple_dmabuf_fops = {
	.owner = THIS_MODULE,
	.open = simple_dmabuf_open,
	.release = simple_dmabuf_release_file,
	.unlocked_ioctl = simple_dmabuf_ioctl,
};

static int simple_dmabuf_probe(struct platform_device *pdev)
{
	struct simple_dmabuf_dev *sdev;
	int ret;

	sdev = devm_kzalloc(&pdev->dev, sizeof(*sdev), GFP_KERNEL);
	if (!sdev)
		return -ENOMEM;

	sdev->dev = &pdev->dev;
	mutex_init(&sdev->lock);

	sdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	sdev->miscdev.name = DEVICE_NAME;
	sdev->miscdev.fops = &simple_dmabuf_fops;

	ret = misc_register(&sdev->miscdev);
	if (ret) {
		dev_err(&pdev->dev, "misc_register failed: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, sdev);

	dev_info(&pdev->dev, "probe success, node is /dev/%s\n", DEVICE_NAME);
	return 0;
}

static int simple_dmabuf_remove(struct platform_device *pdev)
{
	struct simple_dmabuf_dev *sdev = platform_get_drvdata(pdev);

	mutex_lock(&sdev->lock);
	if (sdev->last_dmabuf) {
		dma_buf_put(sdev->last_dmabuf);
		sdev->last_dmabuf = NULL;
	}
	mutex_unlock(&sdev->lock);

	misc_deregister(&sdev->miscdev);

	dev_info(&pdev->dev, "remove success\n");
	return 0;
}

static struct platform_driver simple_dmabuf_driver = {
	.probe = simple_dmabuf_probe,
	.remove = simple_dmabuf_remove,
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = simple_dmabuf_of_match,
	},
};

module_platform_driver(simple_dmabuf_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alientek");
MODULE_DESCRIPTION("Simple platform misc dmabuf exporter teaching template");
