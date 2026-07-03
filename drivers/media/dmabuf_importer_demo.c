#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "dmabuf_importer_demo"

struct dmabuf_import_arg {
	__s32 fd;
	__u32 direction;
};

#define DMABUF_IMPORT_IOC_MAGIC 'i'
#define DMABUF_IMPORT_IOC_MAP \
	_IOW(DMABUF_IMPORT_IOC_MAGIC, 1, struct dmabuf_import_arg)

struct dmabuf_importer_dev {
	struct miscdevice miscdev;
	struct mutex lock;
};

static enum dma_data_direction dmabuf_import_direction(__u32 direction)
{
	switch (direction) {
	case DMA_TO_DEVICE:
	case DMA_FROM_DEVICE:
	case DMA_BIDIRECTIONAL:
		return direction;
	default:
		return DMA_BIDIRECTIONAL;
	}
}

static int dmabuf_importer_map_fd(struct dmabuf_importer_dev *idev,
				  int fd, enum dma_data_direction dir)
{
	struct device *dev = idev->miscdev.this_device;
	struct dma_buf_attachment *attach;
	struct scatterlist *sg;
	struct dma_buf *dmabuf;
	struct sg_table *sgt;
	int i;
	int ret = 0;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		dev_err(dev, "dma_buf_get fd=%d failed: %d\n", fd, ret);
		return ret;
	}

	dev_info(dev, "got dmabuf fd=%d size=%zu dir=%d\n",
		 fd, dmabuf->size, dir);

	attach = dma_buf_attach(dmabuf, dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		dev_err(dev, "dma_buf_attach failed: %d\n", ret);
		goto err_put;
	}

	sgt = dma_buf_map_attachment(attach, dir);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		dev_err(dev, "dma_buf_map_attachment failed: %d\n", ret);
		goto err_detach;
	}

	dev_info(dev, "mapped dmabuf: orig_nents=%u nents=%u\n",
		 sgt->orig_nents, sgt->nents);

	for_each_sgtable_dma_sg(sgt, sg, i) {
		dev_info(dev, "  dma sg[%d]: addr=%pad len=%u\n",
			 i, &sg_dma_address(sg), sg_dma_len(sg));
	}

	/*
	 * 这里只是 importer 教学 demo，所以马上 unmap/detach/put。
	 * 真正硬件驱动会在 map 成功后把 sg_dma_address()/sg_dma_len()
	 * 配置给硬件，等硬件访问完成后再 unmap。
	 */
	dma_buf_unmap_attachment(attach, sgt, dir);
	dma_buf_detach(dmabuf, attach);
	dma_buf_put(dmabuf);
	return 0;

err_detach:
	dma_buf_detach(dmabuf, attach);
err_put:
	dma_buf_put(dmabuf);
	return ret;
}

static int dmabuf_importer_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct dmabuf_importer_dev *idev;

	idev = container_of(miscdev, struct dmabuf_importer_dev, miscdev);
	filp->private_data = idev;
	return 0;
}

static long dmabuf_importer_ioctl(struct file *filp, unsigned int cmd,
				  unsigned long arg)
{
	struct dmabuf_importer_dev *idev = filp->private_data;
	struct dmabuf_import_arg import_arg;
	enum dma_data_direction dir;
	long ret;

	if (_IOC_TYPE(cmd) != DMABUF_IMPORT_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case DMABUF_IMPORT_IOC_MAP:
		if (copy_from_user(&import_arg, (void __user *)arg,
				   sizeof(import_arg)))
			return -EFAULT;

		if (import_arg.fd < 0)
			return -EINVAL;

		dir = dmabuf_import_direction(import_arg.direction);

		mutex_lock(&idev->lock);
		ret = dmabuf_importer_map_fd(idev, import_arg.fd, dir);
		mutex_unlock(&idev->lock);
		return ret;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations dmabuf_importer_fops = {
	.owner = THIS_MODULE,
	.open = dmabuf_importer_open,
	.unlocked_ioctl = dmabuf_importer_ioctl,
};

static struct dmabuf_importer_dev g_importer_dev = {
	.miscdev = {
		.minor = MISC_DYNAMIC_MINOR,
		.name = DEVICE_NAME,
		.fops = &dmabuf_importer_fops,
	},
};

static int __init dmabuf_importer_init(void)
{
	int ret;

	mutex_init(&g_importer_dev.lock);

	ret = misc_register(&g_importer_dev.miscdev);
	if (ret)
		return ret;

	ret = dma_coerce_mask_and_coherent(g_importer_dev.miscdev.this_device,
					   DMA_BIT_MASK(32));
	if (ret) {
		dev_err(g_importer_dev.miscdev.this_device,
			"dma_coerce_mask_and_coherent failed: %d\n", ret);
		misc_deregister(&g_importer_dev.miscdev);
		return ret;
	}

	dev_info(g_importer_dev.miscdev.this_device,
		 "probe success, node is /dev/%s\n", DEVICE_NAME);
	return 0;
}

static void __exit dmabuf_importer_exit(void)
{
	struct device *dev = g_importer_dev.miscdev.this_device;

	dev_info(dev, "remove success\n");
	misc_deregister(&g_importer_dev.miscdev);
}

module_init(dmabuf_importer_init);
module_exit(dmabuf_importer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hjy");
MODULE_DESCRIPTION("Simple dma-buf importer demo");
