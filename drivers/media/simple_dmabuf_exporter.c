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
 *
 * 从“谁调用谁”的角度看，这个文件分成三条线：
 *
 *   A. 应用层申请 dmabuf：
 *
 *      open("/dev/simple_dmabuf_exporter")
 *        -> simple_dmabuf_open()
 *
 *      ioctl(SIMPLE_DMABUF_IOC_EXPORT)
 *        -> simple_dmabuf_ioctl()
 *        -> simple_dmabuf_export_one()
 *        -> dma_alloc_coherent()
 *        -> dma_buf_export()
 *        -> dma_buf_fd()
 *
 *      返回给应用层的是 dmabuf fd，不是物理地址，也不是 dma_addr。
 *
 *   B. 应用层 CPU 访问 dmabuf：
 *
 *      mmap(dmabuf_fd)
 *        -> dma_buf_mmap()
 *        -> simple_dmabuf_mmap()
 *        -> dma_mmap_coherent()
 *
 *      ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, START/END)
 *        -> dma_buf_begin_cpu_access() / dma_buf_end_cpu_access()
 *        -> simple_dmabuf_begin_cpu_access() / simple_dmabuf_end_cpu_access()
 *
 *      这版底层用 coherent 内存，所以 begin/end 只打印日志。
 *      如果以后换成 streaming/cacheable 内存，end_cpu_access(WRITE)
 *      就是你补 sync_for_device 的关键位置。
 *
 *   C. 另一个内核驱动作为 importer 给硬件用：
 *
 *      dmabuf = dma_buf_get(fd)
 *      attach = dma_buf_attach(dmabuf, importer_dev)
 *        -> simple_dmabuf_attach()
 *
 *      sgt = dma_buf_map_attachment(attach, DMA_TO_DEVICE/FROM_DEVICE)
 *        -> simple_dmabuf_map_dma_buf()
 *
 *      importer 从 sgt 里取 DMA 地址，配置给自己的硬件。
 *
 *      用完：
 *      dma_buf_unmap_attachment()
 *        -> simple_dmabuf_unmap_dma_buf()
 *      dma_buf_detach()
 *        -> simple_dmabuf_detach()
 *      dma_buf_put()
 */

#define DEVICE_NAME "simple_dmabuf_exporter"
#define DUMP_SIZE 100

struct simple_dmabuf_export_arg {
	__u32 size;
	__s32 fd;
};
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


struct simple_dmabuf_dev {
	struct device *dev;
	struct miscdevice miscdev;
	struct mutex lock;

	/*
	 * 这是“exporter 设备对象”，对应 /dev/simple_dmabuf_exporter。
	 * 它本身不等于某个 buffer，只是负责响应 ioctl 并创建 buffer。
	 *
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

	/*
	 * backing storage：真正承载数据的内存。
	 *
	 * cpu_addr 给 CPU 用，比如内核 memset、dump，或者 mmap 后用户态访问。
	 * dma_addr 给设备/DMA 控制器用，比如 importer 的硬件读写。
	 *
	 * 这版用 dma_alloc_coherent()，所以 CPU 和设备之间不需要手动
	 * dma_sync_*。如果你以后把它换成 kmalloc + dma_map_single，
	 * begin/end_cpu_access 和 map/unmap_dma_buf 就要认真处理 cache。
	 */
	void *cpu_addr;
	dma_addr_t dma_addr;
	size_t size;

	/*
	 * 这个结构体挂在 dmabuf->priv 上。
	 * 也就是说，dmabuf 框架回调进来的时候，exporter 可以通过
	 * dmabuf->priv 找回自己真正的 buffer 对象。
	 *
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

	/*
	 * sg_table 是 exporter 交给 importer 的“这块 buffer 在 DMA
	 * 世界里长什么样”的描述。
	 *
	 * 简化版只做 1 个 sg entry：
	 *
	 *   sgt->sgl
	 *     page/offset/length  描述 backing page
	 *     dma_address/dma_len 描述 importer 可用于 DMA 的地址和长度
	 *
	 * importer 驱动一般不会直接碰 buf->dma_addr，而是拿这里返回的
	 * sg_table，再从 sg_dma_address(sg) 取地址配置给自己的硬件。
	 */
	sg_dma_address(sgt->sgl) = buf->dma_addr;
	sg_dma_len(sgt->sgl) = buf->size;
	sg_set_page(sgt->sgl, virt_to_page(buf->cpu_addr), buf->size, 0);

	/*
	 * 这一步把 sg_table 映射到 attach->dev 这个 importer 设备的
	 * DMA 地址空间。
	 *
	 * 对没有 IOMMU、地址空间很直接的平台，你可能觉得 dma 地址
	 * 没变化；但在有 IOMMU/SMMU 或 DMA mask 限制时，这一步非常关键。
	 *
	 * direction 表示 importer 设备接下来怎么访问：
	 *   DMA_TO_DEVICE   CPU/exporter 写好，importer 设备读
	 *   DMA_FROM_DEVICE importer 设备写，之后 CPU/exporter 读
	 *   DMA_BIDIRECTIONAL 双向
	 */
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

	/*
	 * importer 设备用完 DMA 地址后，必须 unmap。
	 * 这和 streaming DMA 里的 dma_map_single/dma_unmap_single 是一组
	 * 类似的生命周期概念，只是 dmabuf 这里以 attachment/sg_table
	 * 为单位。
	 */
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

	/*
	 * 调用语义：
	 *   CPU 要开始访问这个 dmabuf 了。
	 *
	 * 如果 direction 是 DMA_FROM_DEVICE，常见含义是：
	 *   设备刚写完，CPU 准备读，所以 exporter 应该让 CPU 看到新数据。
	 *
	 * 对 streaming/cacheable backing storage 来说，这里通常对应
	 * sync_for_cpu/invalidate cache。
	 */
	dev_info(buf->dev, "begin_cpu_access: dir=%d\n", direction);
	return 0;
}

static int simple_dmabuf_end_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction direction)
{
	struct simple_dmabuf_buffer *buf = dmabuf->priv;

	/*
	 * 调用语义：
	 *   CPU 访问结束，buffer 可能要重新交给设备。
	 *
	 * 如果 direction 是 DMA_TO_DEVICE，常见含义是：
	 *   CPU 刚写完，设备准备读，所以 exporter 应该把 CPU cache
	 *   里的新数据刷到设备可见的位置。
	 *
	 * 你之前 USB 摄像头 CPU 拷贝后给 RGA/MPP 读，靠
	 * DMA_BUF_IOCTL_SYNC END WRITE 解决花屏，本质就落在这个语义上。
	 */
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

	/*
	 * 1. 先创建 exporter 自己的 buffer 对象。
	 *
	 * 注意，这还不是 struct dma_buf。
	 * simple_dmabuf_buffer 是这个教学驱动自己的私有结构。
	 */
	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf->dev = sdev->dev;
	buf->size = PAGE_ALIGN(size);
	mutex_init(&buf->lock);

	/*
	 * 2. 分配真正的 DMA backing storage。
	 *
	 * coherent 版本的优点是最容易跑通：
	 *   CPU 可以通过 cpu_addr 访问
	 *   设备可以通过 dma_addr 访问
	 *   begin/end_cpu_access 不需要真的刷 cache
	 *
	 * 它适合教学和小块共享内存。视频大帧/生产链路通常会进一步看
	 * CMA、dma-heap、vb2、ION 或者设备专用 allocator。
	 */
	buf->cpu_addr = dma_alloc_coherent(buf->dev, buf->size,
					   &buf->dma_addr, GFP_KERNEL);
	if (!buf->cpu_addr) {
		kfree(buf);
		return -ENOMEM;
	}

	memset(buf->cpu_addr, 0x00, buf->size);

	/*
	 * 3. 准备 dma_buf_export_info。
	 *
	 * ops  决定外界 mmap、attach、map、sync、release 时回调谁。
	 * size 是 dmabuf 对外暴露的大小。
	 * priv 是 exporter 私有指针，后续所有 dma_buf_ops 都靠
	 *      dmabuf->priv 找回 simple_dmabuf_buffer。
	 */
	exp_info.ops = &simple_dmabuf_ops;
	exp_info.size = buf->size;
	exp_info.flags = O_RDWR;
	exp_info.priv = buf;

	/*
	 * 4. 创建 struct dma_buf。
	 *
	 * 到这里 buffer 已经进入 dma-buf 框架，有了引用计数和 ops，
	 * 但用户态还拿不到它，因为还没有 fd。
	 */
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		dma_free_coherent(buf->dev, buf->size, buf->cpu_addr,
				  buf->dma_addr);
		kfree(buf);
		return PTR_ERR(dmabuf);
	}

	/*
	 * 5. 把 struct dma_buf 安装成当前进程的 fd。
	 *
	 * 这个 fd 可以：
	 *   mmap()
	 *   close()
	 *   传给别的进程
	 *   传给 RGA/DRM/V4L2/MPP 等 importer
	 */
	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0) {
		/*
		 * dma_buf_fd() 失败时，要释放 dma_buf 引用。
		 * release 回调会负责释放 backing buffer。
		 */
		dma_buf_put(dmabuf);
		return fd;
	}

	/*
	 * 6. 教学调试：额外保存最近一次导出的 dmabuf。
	 *
	 * get_dma_buf() 是增加引用。这样即使用户态暂时 close(fd)，
	 * 只要 last_dmabuf 还持有引用，DUMP 仍然能看到它。
	 *
	 * remove() 或下一次 export 时会 dma_buf_put() 放掉这个引用。
	 */
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
		/*
		 * 应用层从 misc 设备 fd 请求“帮我导出一块 dmabuf”。
		 *
		 * 这里有两个 fd：
		 *   filp 对应 /dev/simple_dmabuf_exporter，是控制 fd。
		 *   export_arg.fd 是返回给应用层的 dmabuf fd，是数据 fd。
		 */
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
		/*
		 * DUMP 只是教学调试接口，用来证明：
		 *   用户态 mmap 写入 dmabuf 后，exporter 的 cpu_addr
		 *   能看到同一块 backing storage 的内容。
		 *
		 * 它不是 dmabuf 标准接口，真实 importer 不靠这个拿数据。
		 */
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
