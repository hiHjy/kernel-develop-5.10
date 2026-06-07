#include <linux/device.h>
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
#include <linux/slab.h>
#include <linux/uaccess.h>

/*
 * 这个文件是字符设备 + ioctl + mmap 的教学模板。
 *
 * 它暂时不接 DMA Engine，只做三件事：
 *   1. 通过设备树 compatible 匹配 platform_device。
 *   2. 创建 /dev/dma_chardev_template 字符设备节点。
 *   3. ioctl 申请/释放 src 和 dst 两块 DMA coherent buffer。
 *   4. mmap 把 src/dst 映射给用户态。
 *
 * 后面你可以在 DMA_TEST_IOC_START 里面接 dmaengine_prep_dma_memcpy()，
 * 把 src_dma_addr 搬到 dst_dma_addr。
 *
 * 建议你按这个用户态顺序测试：
 *   1. fd = open("/dev/dma_chardev_template", O_RDWR);
 *   2. ioctl(fd, DMA_TEST_IOC_ALLOC, &alloc_arg);
 *   3. ioctl(fd, DMA_TEST_IOC_INFO, &info_arg);
 *   4. src = mmap(..., fd, info_arg.src_mmap_offset);
 *   5. dst = mmap(..., fd, info_arg.dst_mmap_offset);
 *   6. memset(src, 0x5a, info_arg.size);
 *   7. ioctl(fd, DMA_TEST_IOC_START);
 *   8. 检查 dst 数据。
 *
 * 这个模板的重点不是把功能一次写满，而是让你把下面这条链路走通：
 *
 *   用户态 fd
 *     -> file_operations
 *     -> ioctl 下命令
 *     -> mmap 共享 buffer
 *     -> DMA 地址给硬件使用
 */

#define DEVICE_NAME "dma_chardev_template"
#define DUMP_SIZE 100

/*
 * ioctl 命令号。
 *
 * _IOW: 用户态向内核传数据，例如传入 size。
 * _IOR: 内核向用户态返回数据，例如返回 buffer 信息。
 * _IO : 不带参数，只表示一个动作。
 *
 * 这些宏最终会编码出一个 cmd，用户态 ioctl(fd, cmd, arg) 时，
 * 内核 unlocked_ioctl() 里根据 cmd 分发。
 */
#define DMA_TEST_IOC_MAGIC 'd'
#define DMA_TEST_IOC_ALLOC _IOW(DMA_TEST_IOC_MAGIC, 1, struct dma_test_alloc_arg)
#define DMA_TEST_IOC_FREE  _IO(DMA_TEST_IOC_MAGIC, 2)
#define DMA_TEST_IOC_START _IO(DMA_TEST_IOC_MAGIC, 3)
#define DMA_TEST_IOC_INFO  _IOR(DMA_TEST_IOC_MAGIC, 4, struct dma_test_info_arg)

/*
 * 用户态传给 ALLOC 的参数。
 *
 * size 表示每块 buffer 的大小。模板里会申请两块：
 *   src buffer: 用户态写入源数据
 *   dst buffer: 后面 DMA 写入目标数据
 */
struct dma_test_alloc_arg {
	__u32 size;
};

/*
 * INFO 返回给用户态的信息。
 *
 * mmap 时我们用 offset 区分映射哪一块：
 *   mmap offset = src_mmap_offset -> 映射 src
 *   mmap offset = dst_mmap_offset -> 映射 dst
 *
 * 用户态 mmap 的 offset 参数单位是字节，但必须页对齐。
 */
struct dma_test_info_arg {
	__u32 size;
	__u32 src_mmap_offset;
	__u32 dst_mmap_offset;
};

struct dma_chardev_ctx {
	/*
	 * dev 指向 platform_device 里的 &pdev->dev。
	 *
	 * DMA API 必须知道“是哪一个设备”在做 DMA，因为不同设备可能有
	 * 不同的 DMA mask、IOMMU、cache 一致性属性、设备树 DMA 配置。
	 */
	struct device *dev;

	/*
	 * miscdevice 用来少写一堆字符设备样板代码。
	 *
	 * 传统字符设备通常要写：
	 *   alloc_chrdev_region()
	 *   cdev_init()
	 *   cdev_add()
	 *   class_create()
	 *   device_create()
	 *
	 * miscdevice 把这些封装了。注册成功后会生成 /dev/DEVICE_NAME。
	 * 但它本质仍然是字符设备，仍然走 file_operations。
	 */
	struct miscdevice miscdev;

	/*
	 * lock 保护 ctx 里的 buffer 状态。
	 *
	 * 比如一个线程正在 mmap，另一个线程 ioctl FREE，如果没有锁，
	 * mmap 可能拿到一块刚被释放的 buffer。
	 */
	struct mutex lock;

	/*
	 * src/dst buffer 的三种视角：
	 *
	 *   src_cpu_addr / dst_cpu_addr:
	 *     内核虚拟地址，驱动自己 memset/memcmp 时使用。
	 *
	 *   src_dma_addr / dst_dma_addr:
	 *     DMA 地址，后面给 DMA Engine/硬件使用。
	 *
	 *   用户态 mmap 返回值:
	 *     用户进程虚拟地址，不保存在 ctx 里，由用户态程序自己保存。
	 *
	 * 这三类地址不是同一个概念，不能混用。
	 */
	void *src_cpu_addr;
	void *dst_cpu_addr;
	dma_addr_t src_dma_addr;
	dma_addr_t dst_dma_addr;

	/*
	 * 每块 buffer 的大小。模板里 src 和 dst 大小相同。
	 */
	size_t size;
};

/*
 * 设备树匹配表。
 *
 * 你之前的 DMA 测试节点如果是：
 *
 *   dma-test {
 *       compatible = "hjy,dma_test";
 *       status = "okay";
 *   };
 *
 * 那这个模板驱动也可以直接匹配同一个节点。
 *
 * 真实项目里建议每个驱动用自己的 compatible，例如：
 *   compatible = "hjy,dma-chardev-template";
 *
 * 这里为了方便你接着前面的实验跑，先沿用 "hjy,dma_test"。
 */
static const struct of_device_id dma_template_of_match[] = {
	{ .compatible = "hjy,dma_test" },
	{ }
};
MODULE_DEVICE_TABLE(of, dma_template_of_match);

/*
 * 释放 buffer。
 *
 * dma_alloc_coherent() 申请的内存必须用 dma_free_coherent() 释放。
 * 注意 size、CPU 地址、DMA 地址必须和申请时对应。
 */
static void dma_template_free_buffers(struct dma_chardev_ctx *ctx)
{
	/*
	 * 这里先释放 src，再释放 dst。顺序不是重点，重点是：
	 *   1. 只释放非 NULL 的 buffer。
	 *   2. 释放后立刻清 NULL，防止重复释放。
	 *   3. size 最后清 0，表示当前没有有效 buffer。
	 */
	if (ctx->src_cpu_addr) {
		dma_free_coherent(ctx->dev, ctx->size, ctx->src_cpu_addr,
				  ctx->src_dma_addr);
		ctx->src_cpu_addr = NULL;
		ctx->src_dma_addr = 0;
	}

	if (ctx->dst_cpu_addr) {
		dma_free_coherent(ctx->dev, ctx->size, ctx->dst_cpu_addr,
				  ctx->dst_dma_addr);
		ctx->dst_cpu_addr = NULL;
		ctx->dst_dma_addr = 0;
	}

	ctx->size = 0;
}

/*
 * 申请 src/dst 两块 coherent buffer。
 *
 * coherent buffer 的特点：
 *   1. CPU 可以通过 cpu_addr 访问。
 *   2. DMA 可以通过 dma_addr 访问。
 *   3. 一般不需要你手动 dma_map_single()/dma_sync_single_*()。
 *
 * 它适合先学习 ioctl/mmap/DMA 地址关系。后面再切 streaming DMA，
 * 就能明显感受到 cache 管理的差别。
 */
static int dma_template_alloc_buffers(struct dma_chardev_ctx *ctx, size_t size)
{
	if (!size)
		return -EINVAL;

	/*
	 * 简化模板行为：
	 *   如果用户重复 ALLOC，就先释放旧 buffer，再申请新 buffer。
	 *
	 * 真实项目里你也可以选择：
	 *   - 已经申请过就返回 -EBUSY
	 *   - 或者要求用户先 FREE 再 ALLOC
	 */
	dma_template_free_buffers(ctx);

	/*
	 * 申请源 buffer。
	 *
	 * 返回值 src_cpu_addr 给 CPU 用。
	 * 第三个参数 &src_dma_addr 会被填成 DMA 地址，给硬件用。
	 */
	ctx->src_cpu_addr = dma_alloc_coherent(ctx->dev, size,
					       &ctx->src_dma_addr, GFP_KERNEL);
	if (!ctx->src_cpu_addr)
		return -ENOMEM;

	/*
	 * 申请目标 buffer。
	 *
	 * 如果 dst 申请失败，要把前面已经成功申请的 src 释放掉。
	 * 这就是最基础的错误回收习惯。
	 */
	ctx->dst_cpu_addr = dma_alloc_coherent(ctx->dev, size,
					       &ctx->dst_dma_addr, GFP_KERNEL);
	if (!ctx->dst_cpu_addr) {
		dma_template_free_buffers(ctx);
		return -ENOMEM;
	}

	ctx->size = size;

	/*
	 * 初始化为 0，方便第一次 mmap 后观察。
	 *
	 * 后面你写用户态测试程序时，可以：
	 *   memset(src_user_addr, 0x5a, size);
	 *   memset(dst_user_addr, 0x00, size);
	 */
	memset(ctx->src_cpu_addr, 0x00, size);
	memset(ctx->dst_cpu_addr, 0x00, size);

	dev_info(ctx->dev,
		 "alloc buffers: size=%zu src_cpu=%p src_dma=%pad dst_cpu=%p dst_dma=%pad\n",
		 ctx->size, ctx->src_cpu_addr, &ctx->src_dma_addr,
		 ctx->dst_cpu_addr, &ctx->dst_dma_addr);

	return 0;
}

/*
 * open 回调。
 *
 * 用户态 open("/dev/dma_chardev_template", O_RDWR) 时进入这里。
 * miscdevice 在调用 open 前，会把 filp->private_data 放成
 * struct miscdevice *。
 *
 * 我们通过 container_of() 从 miscdevice 找回自己的 ctx，然后再把
 * filp->private_data 改成 ctx，后面的 ioctl/mmap 就能直接使用。
 */
static int dma_template_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct dma_chardev_ctx *ctx;

	/*
	 * container_of(ptr, type, member) 是内核常用技巧。
	 *
	 * 已知：
	 *   ptr 指向 ctx->miscdev
	 *
	 * 反推：
	 *   ctx 的起始地址
	 *
	 * 因为 miscdev 是 struct dma_chardev_ctx 里的一个成员，所以可以
	 * 通过成员地址反推出外层结构体地址。
	 */
	ctx = container_of(miscdev, struct dma_chardev_ctx, miscdev);
	filp->private_data = ctx;

	return 0;
}

/*
 * release 回调。
 *
 * 用户态 close(fd) 时进入这里。
 * 模板里不在 close 自动释放 buffer，因为你后面可能想多次 open/close
 * 观察生命周期。真正释放由 ioctl FREE 或模块卸载完成。
 */
static int dma_template_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * read 回调。
 *
 * 用户态调用：
 *   read(fd, buf, count);
 *   cat /dev/dma_chardev_template
 *
 * 这个模板里的 read 不把数据 copy_to_user() 返回给应用层，
 * 而是把当前 src/dst buffer 的前 DUMP_SIZE 字节直接打印到内核日志。
 *
 * 这样设计是为了教学调试：
 *   1. 用户态随便 read 一下，就能触发驱动 dump buffer。
 *   2. 不关心用户态传入的 buf/count。
 *   3. 打印结果用 dmesg 查看。
 *
 * 注意：
 *   返回 0 表示 EOF。这样用 cat 读取时，打印一次就会退出，
 *   不会因为反复返回正数而一直循环读。
 */
static ssize_t dma_template_read(struct file *filp, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct dma_chardev_ctx *ctx = filp->private_data;
	size_t dump_size;
	ssize_t ret = 0;

	mutex_lock(&ctx->lock);

	if (!ctx->size || !ctx->src_cpu_addr || !ctx->dst_cpu_addr) {
		dev_err(ctx->dev, "read dump failed: buffer not allocated\n");
		ret = -ENOMEM;
		goto out;
	}

	dump_size = min_t(size_t, ctx->size, DUMP_SIZE);

	dev_info(ctx->dev, "read trigger: dump first %zu bytes\n", dump_size);

	print_hex_dump(KERN_INFO, "dma_chardev src: ",
		       DUMP_PREFIX_OFFSET, 16, 1,
		       ctx->src_cpu_addr, dump_size, true);

	print_hex_dump(KERN_INFO, "dma_chardev dst: ",
		       DUMP_PREFIX_OFFSET, 16, 1,
		       ctx->dst_cpu_addr, dump_size, true);

out:
	mutex_unlock(&ctx->lock);

	/*
	 * 正常 dump 完也返回 0，不给用户态拷贝任何内容。
	 * 应用层真正要看内容，用 dmesg。
	 */
	return ret;
}

/*
 * ioctl 回调。
 *
 * 用户态：
 *   ioctl(fd, DMA_TEST_IOC_ALLOC, &arg);
 *
 * 内核：
 *   cmd 是命令号。
 *   arg 是用户态指针的整数形式，不能直接解引用。
 *
 * 从用户态读结构体要用 copy_from_user()。
 * 往用户态写结构体要用 copy_to_user()。
 */
static long dma_template_ioctl(struct file *filp, unsigned int cmd,
			       unsigned long arg)
{
	struct dma_chardev_ctx *ctx = filp->private_data;
	struct dma_test_alloc_arg alloc_arg;
	struct dma_test_info_arg info_arg;
	long ret = 0;

	/*
	 * _IOC_TYPE(cmd) 会取出命令号里的 magic 字段。
	 *
	 * 这一步相当于确认：
	 *   这个 ioctl 命令是不是发给我们这个驱动的。
	 *
	 * 如果用户传了别的驱动的 cmd，返回 -ENOTTY。
	 */
	if (_IOC_TYPE(cmd) != DMA_TEST_IOC_MAGIC)
		return -ENOTTY;

	/*
	 * ioctl 可能和 mmap、remove、另一个 ioctl 并发。
	 * 这里用一把简单互斥锁保护 buffer 生命周期。
	 */
	mutex_lock(&ctx->lock);

	switch (cmd) {
	case DMA_TEST_IOC_ALLOC:
		/*
		 * 用户态调用示例：
		 *
		 *   struct dma_test_alloc_arg arg = {
		 *       .size = 640 * 480 * 2,
		 *   };
		 *   ioctl(fd, DMA_TEST_IOC_ALLOC, &arg);
		 *
		 * arg 是用户态地址，内核不能直接 *(struct xxx *)arg。
		 * 必须用 copy_from_user() 拷贝到内核栈变量 alloc_arg。
		 */
		if (copy_from_user(&alloc_arg, (void __user *)arg,
				   sizeof(alloc_arg))) {
			ret = -EFAULT;
			break;
		}

		ret = dma_template_alloc_buffers(ctx, alloc_arg.size);
		break;

	case DMA_TEST_IOC_FREE:
		/*
		 * 用户态调用示例：
		 *
		 *   ioctl(fd, DMA_TEST_IOC_FREE);
		 *
		 * 注意：
		 *   如果用户态还持有 mmap 地址，FREE 后继续访问那段地址是
		 *   错误行为。真实项目里要更严谨地管理引用计数和映射状态。
		 *   这个模板先保持简单。
		 */
		dma_template_free_buffers(ctx);
		break;

	case DMA_TEST_IOC_INFO:
		/*
		 * 用户态调用示例：
		 *
		 *   struct dma_test_info_arg info;
		 *   ioctl(fd, DMA_TEST_IOC_INFO, &info);
		 *
		 * 返回后用户态知道：
		 *   info.size            每块 buffer 多大
		 *   info.src_mmap_offset mmap src 时用的 offset
		 *   info.dst_mmap_offset mmap dst 时用的 offset
		 */
		info_arg.size = ctx->size;
		info_arg.src_mmap_offset = 0;
		info_arg.dst_mmap_offset = PAGE_ALIGN(ctx->size);

		/*
		 * copy_to_user() 把内核栈变量 info_arg 拷贝回用户态。
		 * 返回非 0 表示还有字节没拷成功，所以按错误处理。
		 */
		if (copy_to_user((void __user *)arg, &info_arg,
				 sizeof(info_arg)))
			ret = -EFAULT;
		break;

	case DMA_TEST_IOC_START:
		/*
		 * 这里先留空，后面接 DMA Engine。
		 *
		 * 你后面要做的事情大概是：
		 *   1. 申请 dma_chan。
		 *   2. 用 ctx->src_dma_addr 和 ctx->dst_dma_addr 准备 descriptor。
		 *   3. submit + issue_pending。
		 *   4. 等 completion。
		 *
		 * coherent buffer 版本一般不用手动 sync cache。
		 *
		 * 用户态调用示例：
		 *
		 *   memset(src_user, 0x5a, size);
		 *   ioctl(fd, DMA_TEST_IOC_START);
		 *   然后检查 dst_user。
		 *
		 * 后面你接 DMA Engine 时，这个 case 最好拆成几个小函数：
		 *   dma_template_request_chan()
		 *   dma_template_do_memcpy()
		 *   dma_template_release_chan()
		 */
		dev_info(ctx->dev, "START placeholder: connect DMA Engine here\n");
		ret = -ENOSYS;
		break;

	default:
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&ctx->lock);
	return ret;
}

/*
 * mmap 回调。
 *
 * 用户态 mmap(fd) 时进入这里。这里把内核申请的 coherent buffer
 * 映射到用户态虚拟地址。
 *
 * 我们用 vma->vm_pgoff 区分用户想映射哪块 buffer：
 *   offset = 0                  -> src
 *   offset = PAGE_ALIGN(size)   -> dst
 *
 * 用户态 mmap 的 offset 参数最终会变成 vm_pgoff，单位是页。
 */
/*
 * 你现在理解 mmap，先抓住 vm_area_struct 里的几个字段就够了：
 *
 *   vma->vm_start:
 *     用户进程虚拟地址起点。mmap 成功后，用户态拿到的地址就在这里。
 *
 *   vma->vm_end:
 *     用户进程虚拟地址终点。vm_end - vm_start 就是本次映射大小。
 *
 *   vma->vm_pgoff:
 *     用户态 mmap 最后一个参数 offset 转成“页”为单位后的值。
 *     所以要用 vma->vm_pgoff << PAGE_SHIFT 转回字节 offset。
 *
 *   vma->vm_flags:
 *     映射权限和属性，例如是否可读、可写、共享等。
 *
 * mmap 回调的核心工作不是拷贝数据，而是建立页表映射：
 *
 *   用户态虚拟地址
 *       -> 同一块 DMA buffer
 *   内核虚拟地址 ctx->src_cpu_addr
 *       -> 同一块 DMA buffer
 *   DMA 地址 ctx->src_dma_addr
 *       -> 同一块 DMA buffer
 */
static int dma_template_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct dma_chardev_ctx *ctx = filp->private_data;
	/*
	 * 用户态请求映射的大小。
	 *
	 * 用户态：
	 *   mmap(NULL, length, ..., fd, offset);
	 *
	 * 内核：
	 *   req_size = length 按页对齐后的 VMA 大小。
	 */
	unsigned long req_size = vma->vm_end - vma->vm_start;

	/*
	 * 用户态 mmap 的 offset 必须页对齐。
	 *
	 * 内核保存到 vma->vm_pgoff 时，已经把字节 offset 除以 PAGE_SIZE。
	 * 所以这里左移 PAGE_SHIFT，把它还原成字节 offset。
	 */
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	void *cpu_addr;
	dma_addr_t dma_addr;
	int ret;

	/*
	 * mmap 也要拿锁，因为它要读取 ctx->size/src/dst 地址。
	 * 防止和 ioctl FREE/ALLOC 并发打架。
	 */
	mutex_lock(&ctx->lock);

	/*
	 * 要求用户必须先 ioctl ALLOC，再 mmap。
	 *
	 * 如果还没有申请 buffer，ctx->size 是 0，src/dst 地址也是 NULL，
	 * 这时没有任何物理页可以映射给用户态。
	 */
	if (!ctx->size || !ctx->src_cpu_addr || !ctx->dst_cpu_addr) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * 用户态一次 mmap 只能映射一块 buffer，不能超过 buffer 大小。
	 *
	 * 例如 size=4096，那么 mmap length 最大就是 4096。
	 */
	if (req_size > ctx->size) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * 用 offset 区分映射 src 还是 dst。
	 *
	 * 用户态映射 src：
	 *   mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	 *
	 * 用户态映射 dst：
	 *   mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	 *        PAGE_ALIGN(size));
	 *
	 * 为什么 dst offset 不随便写成 1？
	 *   mmap offset 必须是 PAGE_SIZE 对齐的字节偏移。
	 */
	if (offset == 0) {
		cpu_addr = ctx->src_cpu_addr;
		dma_addr = ctx->src_dma_addr;
	} else if (offset == PAGE_ALIGN(ctx->size)) {
		cpu_addr = ctx->dst_cpu_addr;
		dma_addr = ctx->dst_dma_addr;
	} else {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * 真正建立映射的是 dma_mmap_coherent()。
	 *
	 * 它知道 dma_alloc_coherent() 得到的 buffer 应该如何映射给用户态。
	 * 成功后，用户态 mmap() 返回的地址、这里的 cpu_addr、dma_addr
	 * 描述的是同一块底层内存的三种视角。
	 *
	 * 注意最后一个参数这里传 ctx->size，而不是 req_size。
	 * 很多驱动会传完整 buffer 大小。用户态本次 VMA 大小由 vma 描述，
	 * dma_mmap_coherent() 内部会按 vma 建映射。
	 */
	vma->vm_pgoff = 0;
	ret = dma_mmap_coherent(ctx->dev, vma, cpu_addr, dma_addr, ctx->size);

out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static const struct file_operations dma_template_fops = {
	/*
	 * owner = THIS_MODULE 可以防止模块正在被使用时被卸载。
	 */
	.owner = THIS_MODULE,

	/*
	 * 用户态 open("/dev/dma_chardev_template", O_RDWR)。
	 */
	.open = dma_template_open,

	/*
	 * 用户态 close(fd)。
	 */
	.release = dma_template_release,

	/*
	 * 用户态 read(fd, buf, count)。
	 * 这里用作调试触发器：不返回用户数据，只打印 src/dst。
	 */
	.read = dma_template_read,

	/*
	 * 用户态 ioctl(fd, cmd, arg)。
	 */
	.unlocked_ioctl = dma_template_ioctl,

	/*
	 * 用户态 mmap(NULL, size, prot, MAP_SHARED, fd, offset)。
	 */
	.mmap = dma_template_mmap,
};

static int dma_template_probe(struct platform_device *pdev)
{
	struct dma_chardev_ctx *ctx;
	int ret;

	/*
	 * 这里的 &pdev->dev 来自设备树创建出来的 platform_device。
	 *
	 * 后面做 DMA 时，dma_alloc_coherent()/dma_map_single() 都应该
	 * 使用这个 dev，而不是随便拿一个 class/misc 的 device。
	 *
	 * 原因：
	 *   1. 这个 dev 绑定了设备树节点 of_node。
	 *   2. 这个 dev 才代表你的这个逻辑设备。
	 *   3. 后面如果设备树里加 dmas/dma-names，也要从这个 dev 取。
	 */
	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = &pdev->dev;
	mutex_init(&ctx->lock);

	ctx->miscdev.minor = MISC_DYNAMIC_MINOR;
	ctx->miscdev.name = DEVICE_NAME;
	ctx->miscdev.fops = &dma_template_fops;

	/*
	 * misc_register() 成功后：
	 *   1. 内核分配一个动态 minor。
	 *   2. 创建设备节点 /dev/dma_chardev_template。
	 *   3. 用户态访问这个节点时，会走 dma_template_fops。
	 */
	ret = misc_register(&ctx->miscdev);
	if (ret) {
		dev_err(&pdev->dev, "misc_register failed: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, ctx);

	dev_info(&pdev->dev, "probe success, device node is /dev/%s\n",
		 DEVICE_NAME);
	return 0;
}

static int dma_template_remove(struct platform_device *pdev)
{
	struct dma_chardev_ctx *ctx = platform_get_drvdata(pdev);

	/*
	 * remove 是 probe 的反向流程：
	 *   probe 里注册 miscdevice，remove 里注销。
	 *   probe/运行中申请 buffer，remove 里释放。
	 */
	mutex_lock(&ctx->lock);
	dma_template_free_buffers(ctx);
	mutex_unlock(&ctx->lock);

	misc_deregister(&ctx->miscdev);

	dev_info(&pdev->dev, "remove success\n");
	return 0;
}

static struct platform_driver dma_template_driver = {
	.probe = dma_template_probe,
	.remove = dma_template_remove,
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = dma_template_of_match,
	},
};

module_platform_driver(dma_template_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alientek");
MODULE_DESCRIPTION("RK3568 DMA char device ioctl/mmap teaching template");
