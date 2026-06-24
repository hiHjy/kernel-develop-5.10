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
 * Simple dma-buf exporter skeleton.
 *
 * 你手敲时按这三条线补：
 *
 * 1. 用户态申请 dmabuf
 *    open("/dev/simple_dmabuf_exporter")
 *    ioctl(SIMPLE_DMABUF_IOC_EXPORT)
 *      -> simple_dmabuf_export_one()
 *      -> dma_alloc_coherent()
 *      -> dma_buf_export()
 *      -> dma_buf_fd()
 *
 * 2. 用户态 CPU 访问 dmabuf
 *    mmap(dmabuf_fd)
 *      -> simple_dmabuf_mmap()
 *
 *    ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC START/END)
 *      -> simple_dmabuf_begin_cpu_access()
 *      -> simple_dmabuf_end_cpu_access()
 *
 * 3. 内核 importer 给硬件用
 *    dma_buf_attach()
 *      -> simple_dmabuf_attach()
 *
 *    dma_buf_map_attachment()
 *      -> simple_dmabuf_map_dma_buf()
 *
 *    dma_buf_unmap_attachment()
 *      -> simple_dmabuf_unmap_dma_buf()
 */

#define DEVICE_NAME "simple_dmabuf_exporter"
#define DUMP_SIZE 100

struct simple_dmabuf_export_arg {
	__u32 size;
	__s32 fd;
};

#define SIMPLE_DMABUF_IOC_MAGIC 'b'
#define SIMPLE_DMABUF_IOC_EXPORT                                               \
	_IOWR(SIMPLE_DMABUF_IOC_MAGIC, 1, struct simple_dmabuf_export_arg)
#define SIMPLE_DMABUF_IOC_DUMP _IO(SIMPLE_DMABUF_IOC_MAGIC, 2)

struct simple_dmabuf_dev {
	struct device *dev;
	struct miscdevice miscdev;
	struct mutex lock;

	/* 教学调试用：保存最近一次导出的 dmabuf。 */
	struct dma_buf *last_dmabuf;
};

struct simple_dmabuf_buffer {
	struct device *dev;
	struct mutex lock;

	/* backing storage: exporter 真正拥有的内存。 */
	void *cpu_addr;
	dma_addr_t dma_addr;
	size_t size;

	bool released;
};

static void simple_dmabuf_dump_buffer(struct simple_dmabuf_buffer *buf,
				      const char *reason)
{
	size_t dump_size;

	if (!buf || !buf->cpu_addr)
		return;

	dump_size = min_t(size_t, buf->size, DUMP_SIZE);

	dev_info(buf->dev,
		 "%s: dump first %zu bytes, cpu=%p dma=%pad size=%zu\n", reason,
		 dump_size, buf->cpu_addr, &buf->dma_addr, buf->size);

	print_hex_dump(KERN_INFO, "simple_dmabuf: ", DUMP_PREFIX_OFFSET, 16, 1,
		       buf->cpu_addr, dump_size, true);
}

static int simple_dmabuf_attach(struct dma_buf *dmabuf,
				struct dma_buf_attachment *attach)
{
	/* TODO: importer 调 dma_buf_attach() 时进入这里。 */
	return 0;
}

static void simple_dmabuf_detach(struct dma_buf *dmabuf,
				 struct dma_buf_attachment *attach)
{
	/* TODO: importer 不再使用 dmabuf 时进入这里。 */
}

static struct sg_table *
simple_dmabuf_map_dma_buf(struct dma_buf_attachment *attach,
			  enum dma_data_direction direction)
{
	/*
	 * TODO:
	 * 1. 从 attach->dmabuf->priv 取回 simple_dmabuf_buffer。
	 * 2. kzalloc 一个 struct sg_table。
	 * 3. 用 dma_get_sgtable_attrs() 从 backing storage 得到 sg_table。
	 * 4. 用 dma_map_sgtable() 映射到 attach->dev 这个 importer 设备。
	 * 5. 成功后返回 sgt。
	 *
	 * 注意：
	 * sg_table 先描述 page/offset/length；
	 * dma_map_sgtable() 成功后，sg_dma_address()/sg_dma_len()
	 * 才是 importer 设备可用的 DMA 视角。
	 */

	struct simple_dmabuf_buffer *buf = attach->dmabuf->priv;
	struct sg_table *sgt;
	int ret;

	dev_info(buf->dev, "map_dma_buf: importer=%s dir=%d\n",
		 dev_name(attach->dev), direction);

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		return ERR_PTR(-ENOMEM);
	}

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (ret) {
		goto err_free_sgt;
	}

	sg_set_buf(sgt->sgl, buf->cpu_addr, buf->size);
	ret = dma_map_sgtable(attach->dev, sgt, direction, 0);

	if (ret) {
		goto err_free_table;
	}

	dev_info(buf->dev,
		 "map ok: importer=%s orig_nents=%u nents=%u dma=%pad len=%u\n",
		 dev_name(attach->dev), sgt->orig_nents, sgt->nents,
		 &sg_dma_address(sgt->sgl), sg_dma_len(sgt->sgl));

	return sgt;
err_free_table:
	sg_free_table(sgt);
err_free_sgt:
	kfree(sgt);
	return ERR_PTR(ret);
}

static void simple_dmabuf_unmap_dma_buf(struct dma_buf_attachment *attach,
					struct sg_table *sgt,
					enum dma_data_direction direction)
{
	/*
	 * TODO:
	 * 1. dma_unmap_sgtable()
	 * 2. sg_free_table()
	 * 3. kfree(sgt)
	 */

	struct simple_dmabuf_buffer *buf = attach->dmabuf->priv;
	dev_info(buf->dev,
		 "unmap_dma_buf: importer=%s orig_nents=%u nents=%u dir=%d\n",
		 dev_name(attach->dev), sgt->orig_nents, sgt->nents, direction);
	dma_unmap_sgtable(attach->dev, sgt, direction, 0);
	sg_free_table(sgt);
	kfree(sgt);
}

static int simple_dmabuf_mmap(struct dma_buf *dmabuf,
			      struct vm_area_struct *vma)
{
	/*
	 * TODO:
	 * 1. 从 dmabuf->priv 取回 simple_dmabuf_buffer。
	 * 2. 检查 vma 请求大小不能超过 buf->size。
	 * 3. 用 dma_mmap_coherent() 映射给用户态。
	 */
	return -ENOSYS;
}

static int simple_dmabuf_begin_cpu_access(struct dma_buf *dmabuf,
					  enum dma_data_direction direction)
{
	struct simple_dmabuf_buffer *buf = dmabuf->priv;

	dev_info(buf->dev, "begin_cpu_access: dir=%d\n", direction);

	if (buf->dma_addr)
		dma_sync_single_for_cpu(buf->dev, buf->dma_addr, buf->size,
					direction);

	return 0;
}

static int simple_dmabuf_end_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction direction)
{
	struct simple_dmabuf_buffer *buf = dmabuf->priv;

	dev_info(buf->dev, "end_cpu_access: dir=%d\n", direction);

	if (buf->dma_addr)
		dma_sync_single_for_device(buf->dev, buf->dma_addr, buf->size,
					   direction);

	return 0;
}

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

	if (buf->dma_addr)
		dma_unmap_single(buf->dev, buf->dma_addr, buf->size,
				 DMA_BIDIRECTIONAL);
	kfree(buf->cpu_addr);
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

static int simple_dmabuf_export_one(struct simple_dmabuf_dev *sdev, size_t size)
{
	/*
	 * TODO:
	 * 1. 检查 size。
	 * 2. kzalloc simple_dmabuf_buffer。
	 * 3. PAGE_ALIGN(size)，初始化 mutex/dev/size。
	 * 4. dma_alloc_coherent() 分配 backing storage。
	 * 5. 填 DEFINE_DMA_BUF_EXPORT_INFO。
	 * 6. dma_buf_export() 创建 struct dma_buf。
	 * 7. dma_buf_fd() 变成用户态 fd。
	 * 8. 可选：get_dma_buf() 保存到 sdev->last_dmabuf，方便 DUMP。
	 */
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	void *ptr;
	dma_addr_t dma_addr;
	struct simple_dmabuf_buffer *buf;

	struct dma_buf *dma_buf;
	int fd;

	if (size <= 0) {
		return -EINVAL;
	}
	size = PAGE_ALIGN(size);

	ptr = kmalloc(size, GFP_KERNEL);
	if (!ptr) {
		dev_err(sdev->dev, "kmalloc err\n");
		return -ENOMEM;
	}
	memset(ptr, 0x00, size);

	dma_addr = dma_map_single(sdev->dev, ptr, size, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(sdev->dev, dma_addr)) {
		dev_err(sdev->dev, "dma_map_single failed\n");
		kfree(ptr);
		return -ENOMEM;
	}

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		dev_err(sdev->dev, "kmalloc simple_dmabuf_buffer err\n");
		dma_unmap_single(sdev->dev, dma_addr, size, DMA_BIDIRECTIONAL);
		kfree(ptr);
		return -ENOMEM;
	}

	buf->dev = sdev->dev;
	buf->cpu_addr = ptr;
	buf->dma_addr = dma_addr;
	buf->size = size;
	mutex_init(&buf->lock);

	exp_info.exp_name = "hjy,dmabuf";
	exp_info.size = size;
	exp_info.flags = O_RDWR;
	exp_info.ops = &simple_dmabuf_ops;
	exp_info.priv = buf;

	dma_buf = dma_buf_export(&exp_info);
	if (IS_ERR(dma_buf)) {
		int err = PTR_ERR(dma_buf);
		dma_unmap_single(sdev->dev, dma_addr, size, DMA_BIDIRECTIONAL);
		kfree(buf);
		kfree(ptr);
		return err;
	}

	fd = dma_buf_fd(dma_buf, O_CLOEXEC);
	if (fd < 0) {
		dma_buf_put(dma_buf);
		return fd;
	}

	mutex_lock(&sdev->lock);
	if (sdev->last_dmabuf)
		dma_buf_put(sdev->last_dmabuf);
	get_dma_buf(dma_buf);
	sdev->last_dmabuf = dma_buf;
	mutex_unlock(&sdev->lock);

	dev_info(sdev->dev, "export dmabuf: size=%zu fd=%d cpu=%p dma=%pad\n",
		 size, fd, buf->cpu_addr, &buf->dma_addr);

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
				 sizeof(export_arg)))
			return -EFAULT;

		return 0;

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
		return 0;

	default:
		return -ENOTTY;
	}
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

static const struct of_device_id simple_dmabuf_of_match[] = {
	{ .compatible = "hjy,dma_test" },
	{}
};
MODULE_DEVICE_TABLE(of, simple_dmabuf_of_match);

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
MODULE_AUTHOR("hjy");
MODULE_DESCRIPTION("Simple dma-buf exporter skeleton");
