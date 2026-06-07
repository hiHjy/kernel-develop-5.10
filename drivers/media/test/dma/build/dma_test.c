#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/string.h>

#define DEVICE_NAME "dma_test"
#define DMA_SIZE 640 * 480 * 2
#define DUMP_SIZE 100

struct test_dev {
	struct device *dev;

	/*
	 * CPU 虚拟地址和 DMA 地址是学习 DMA 时最容易混的点。
	 *
	 * src_cpu_addr / dst_cpu_addr:
	 *   CPU 使用的虚拟地址。CPU 想 memset、memcmp、读写内存时用它。
	 *
	 * src_dma_addr / dst_dma_addr:
	 *   DMA 控制器使用的 DMA 地址。硬件搬运数据时用它。
	 *
	 * 重点：
	 *   1. 不要把 cpu_addr 交给 DMA 硬件。
	 *   2. 不要把 dma_addr 当 CPU 指针解引用。
	 *   3. 它们描述的是同一块内存的两个视角。
	 */
	void *src_cpu_addr;
	void *dst_cpu_addr;
	dma_addr_t src_dma_addr;
	dma_addr_t dst_dma_addr;
	/*
	 * DMA Engine 通道。
	 *
	 * 这里不是直接 ioremap PL330 寄存器，也不是自己写寄存器。
	 * Linux 推荐通过 DMA Engine 框架申请抽象通道。
	 *
	 * 在你的 RK3568 板子上，背后的硬件提供者就是日志里的：
	 *
	 *   dma-pl330 fe530000.dmac
	 *   dma-pl330 fe550000.dmac
	 *
	 * 也就是说，我们调用的是通用 DMA Engine API，真正干活的是
	 * drivers/dma/pl330.c 驱动和 PL330 DMAC 硬件。
	 */
	struct dma_chan *chan;

	/*
	 * completion 用来等待 DMA 完成。
	 *
	 * DMA 是异步的：
	 *   调用 dma_async_issue_pending() 之后，CPU 不会自动阻塞等 DMA。
	 *
	 * 所以这里用 completion 做同步：
	 *   1. probe 里 wait_for_completion_timeout() 睡眠等待。
	 *   2. DMA 完成回调里 complete() 唤醒等待者。
	 */
	struct completion dma_done;
};

/*
 * DMA 完成回调函数。
 *
 * DMA Engine 在本次 descriptor 搬运完成后会调用它。
 * 回调函数里不要做复杂事情，保持短小。
 * 这里仅仅唤醒等待 DMA 完成的 probe 流程。
 */
static void dma_test_complete_func(void *arg)
{
	complete(arg);
}

/*
 * 打印 src/dst 前 100 字节。
 *
 * 搬运前：
 *   src 应该全是 0x5a。
 *   dst 应该全是 0x00。
 *
 * 搬运后：
 *   src 还是 0x5a。
 *   dst 应该也变成 0x5a。
 *
 * print_hex_dump() 会以十六进制形式打印内存，适合观察 DMA 搬运前后
 * 数据是否真的发生变化。
 */
static void dma_test_dump_100(struct device *dev, const char *stage,
			      const void *src, const void *dst)
{
	dev_info(dev, "%s: dump first %d bytes\n", stage, DUMP_SIZE);

	print_hex_dump(KERN_INFO, "dma_test src: ", DUMP_PREFIX_OFFSET,
		       16, 1, src, DUMP_SIZE, true);
	print_hex_dump(KERN_INFO, "dma_test dst: ", DUMP_PREFIX_OFFSET,
		       16, 1, dst, DUMP_SIZE, true);
}

/*
 * 设备树匹配表。
 *
 * 你的设备树节点里需要有：
 *
 *   compatible = "hjy,dma_test";
 *
 * platform 总线根据设备树创建 platform_device 后，会用 compatible 匹配
 * 这个驱动。匹配成功后，内核调用 dma_test_probe()。
 */
static const struct of_device_id dma_test_of_match[] = {
	{ .compatible = "hjy,dma_test" },
	{}
};
MODULE_DEVICE_TABLE(of, dma_test_of_match);

static int dma_test_probe(struct platform_device *pdev)
{
	struct test_dev *tdev;
	dma_cap_mask_t mask;
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	enum dma_status status;
	unsigned long timeout;
	int ret = 0;

	dev_info(&pdev->dev, "probe start\n");
	dev_info(&pdev->dev, "sizeof(dma_addr_t) = %zu\n", sizeof(dma_addr_t));

	/*
	 * 申请驱动私有数据。
	 *
	 * devm_kzalloc() 可以理解成带自动释放能力的 kzalloc：
	 *   kzalloc: 申请的内存会清零。
	 *   devm: 设备移除时由 devres 机制自动释放。
	 *
	 * 注意：
	 *   这里 tdev 是 devm 管理的。
	 *   但 DMA buffer 和 DMA channel 不是 devm 申请的，所以后面仍然
	 *   要手动 dma_free_coherent() 和 dma_release_channel()。
	 */
	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	tdev->dev = &pdev->dev;
	platform_set_drvdata(pdev, tdev);

	/*
	 * 申请 DMA coherent 源 buffer。
	 *
	 * dma_alloc_coherent() 会返回两个地址：
	 *
	 *   tdev->src_cpu_addr:
	 *     CPU 访问这块内存用的虚拟地址。
	 *
	 *   tdev->src_dma_addr:
	 *     DMA 控制器访问这块内存用的 DMA 地址。
	 *
	 * coherent 的意思：
	 *   这块内存对 CPU 和 DMA 设备保持一致性。
	 *   对这个入门测试来说，我们不用额外调用 dma_sync_* 做 cache 同步。
	 */
	
	tdev->src_cpu_addr = dma_alloc_coherent(tdev->dev, DMA_SIZE,
						&tdev->src_dma_addr,
						GFP_KERNEL);
	if (!tdev->src_cpu_addr)
		return -ENOMEM;

	/*
	 * 申请 DMA coherent 目标 buffer。
	 *
	 * 本次 DMA 要做的是：
	 *
	 *   src_dma_addr -> dst_dma_addr
	 *
	 * CPU 最后验证的是：
	 *
	 *   src_cpu_addr 和 dst_cpu_addr 内容是否一致
	 */
	tdev->dst_cpu_addr = dma_alloc_coherent(tdev->dev, DMA_SIZE,
						&tdev->dst_dma_addr,
						GFP_KERNEL);
	if (!tdev->dst_cpu_addr) {
		ret = -ENOMEM;
		goto free_src;
	}

	/*
	 * 准备测试数据。
	 *
	 * 源 buffer 全部填成 0x5a。
	 * 目标 buffer 全部清成 0x00。
	 *
	 * 如果 DMA 搬运成功，dst 的内容会从 0x00 变成 0x5a。
	 */
	memset(tdev->src_cpu_addr, 0x5a, DMA_SIZE);
	memset(tdev->dst_cpu_addr, 0x00, DMA_SIZE);

	/*
	 * 搬运前打印前 100 字节。
	 *
	 * 你应该能在 dmesg 里看到：
	 *   src: 5a 5a 5a ...
	 *   dst: 00 00 00 ...
	 */
	dma_test_dump_100(tdev->dev, "before dma memcpy",
			  tdev->src_cpu_addr, tdev->dst_cpu_addr);

	/*
	 * 申请一个支持内存到内存搬运的 DMA channel。
	 *
	 * dma_cap_mask_t:
	 *   用来描述我们想要的 DMA 能力。
	 *
	 * DMA_MEMCPY:
	 *   表示我们要做 memory-to-memory copy。
	 *
	 * dma_request_channel(mask, NULL, NULL):
	 *   按 capability 从系统里找一个可用 DMA channel。
	 *
	 * 这里没有写 dmas = <&dmac0 N>，因为我们不是在测 UART/I2S/SPI
	 * 那种外设请求线，而是在测 DMA Engine 是否提供 memcpy 能力。
	 */
	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	tdev->chan = dma_request_channel(mask, NULL, NULL);
	if (!tdev->chan) {
		dev_err(tdev->dev, "no DMA_MEMCPY channel\n");
		ret = -ENODEV;
		goto free_dst;
	}

	init_completion(&tdev->dma_done);

	/*
	 * 准备一个 DMA memcpy descriptor。
	 *
	 * descriptor 可以理解成“一次 DMA 任务描述”。
	 * 这里只是描述任务，还没有真正启动 DMA。
	 *
	 * 参数顺序非常重要：
	 *   1. DMA channel
	 *   2. 目标 DMA 地址
	 *   3. 源 DMA 地址
	 *   4. 搬运长度
	 *   5. flags
	 *
	 * DMA_PREP_INTERRUPT:
	 *   请求 DMA 完成后触发回调。
	 *
	 * DMA_CTRL_ACK:
	 *   告诉 DMA Engine 这个 descriptor 按正常流程确认和回收。
	 */
	desc = dmaengine_prep_dma_memcpy(tdev->chan,
					 tdev->dst_dma_addr,
					 tdev->src_dma_addr,
					 DMA_SIZE,
					 DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc) {
		dev_err(tdev->dev, "prep dma memcpy failed\n");
		ret = -EIO;
		goto release_chan;
	}

	/*
	 * 设置 DMA 完成回调。
	 *
	 * 当 DMA 任务完成后，DMA Engine 会调用：
	 *
	 *   dma_test_complete_func(&tdev->dma_done)
	 *
	 * 然后 probe 里等待 completion 的地方会被唤醒。
	 */
	desc->callback = dma_test_complete_func;
	desc->callback_param = &tdev->dma_done;

	/*
	 * 提交 descriptor。
	 *
	 * dmaengine_submit() 把 descriptor 交给 DMA Engine 队列，并返回
	 * 一个 cookie。cookie 类似本次 DMA 任务的 id，后面可以用它查询
	 * 传输状态。
	 */
	cookie = dmaengine_submit(desc);
	ret = dma_submit_error(cookie);
	if (ret) {
		dev_err(tdev->dev, "submit dma failed: %d\n", ret);
		goto release_chan;
	}

	/*
	 * 真正启动 DMA。
	 *
	 * submit 只是入队，issue_pending 才会推动 pending 队列开始执行。
	 */
	dma_async_issue_pending(tdev->chan);

	/*
	 * 等待 DMA 完成回调。
	 *
	 * 这里设置 1000 ms 超时，避免硬件或驱动异常时 probe 永久卡死。
	 */
	timeout = wait_for_completion_timeout(&tdev->dma_done,
					      msecs_to_jiffies(1000));
	if (!timeout) {
		dmaengine_terminate_sync(tdev->chan);
		dev_err(tdev->dev, "dma memcpy timeout\n");
		ret = -ETIMEDOUT;
		goto release_chan;
	}

	/*
	 * 查询 DMA 任务状态。
	 *
	 * callback 已经唤醒了 completion，但这里再查一次 cookie 状态，
	 * 是为了演示 DMA Engine 的状态查询 API。
	 */
	status = dma_async_is_tx_complete(tdev->chan, cookie, NULL, NULL);
	if (status != DMA_COMPLETE) {
		dev_err(tdev->dev, "dma memcpy incomplete, status=%d\n", status);
		ret = -EIO;
		goto release_chan;
	}

	/*
	 * 搬运后打印前 100 字节。
	 *
	 * 你应该能在 dmesg 里看到：
	 *   src: 5a 5a 5a ...
	 *   dst: 5a 5a 5a ...
	 */
	dma_test_dump_100(tdev->dev, "after dma memcpy",
			  tdev->src_cpu_addr, tdev->dst_cpu_addr);

	/*
	 * 最终数据校验。
	 *
	 * 如果 DMA 确实完成了 src -> dst 搬运，两个 buffer 内容应该完全一致。
	 */
	if (memcmp(tdev->src_cpu_addr, tdev->dst_cpu_addr, DMA_SIZE)) {
		dev_err(tdev->dev, "dma memcpy data mismatch\n");
		ret = -EIO;
		goto release_chan;
	}

	dev_info(tdev->dev, "src_cpu_addr=%p src_dma_addr=%pad size=%d\n",
		 tdev->src_cpu_addr, &tdev->src_dma_addr, DMA_SIZE);
	dev_info(tdev->dev, "dst_cpu_addr=%p dst_dma_addr=%pad size=%d\n",
		 tdev->dst_cpu_addr, &tdev->dst_dma_addr, DMA_SIZE);
	dev_info(tdev->dev, "dma memcpy test success\n");
	dev_info(tdev->dev, "dma_test_probe success\n");
	return 0;

release_chan:
	dma_release_channel(tdev->chan);
	tdev->chan = NULL;
free_dst:
	dma_free_coherent(tdev->dev, DMA_SIZE, tdev->dst_cpu_addr,
			  tdev->dst_dma_addr);
free_src:
	dma_free_coherent(tdev->dev, DMA_SIZE, tdev->src_cpu_addr,
			  tdev->src_dma_addr);
	return ret;
}

static int dma_test_remove(struct platform_device *pdev)
{
	struct test_dev *tdev = platform_get_drvdata(pdev);

	dev_info(tdev->dev, "remove\n");

	if (tdev->chan)
		dma_release_channel(tdev->chan);

	dma_free_coherent(tdev->dev, DMA_SIZE, tdev->src_cpu_addr,
			  tdev->src_dma_addr);
	dma_free_coherent(tdev->dev, DMA_SIZE, tdev->dst_cpu_addr,
			  tdev->dst_dma_addr);

	return 0;
}

static struct platform_driver dma_test_driver = {
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = dma_test_of_match,
	},
	.probe = dma_test_probe,
	.remove = dma_test_remove,
};

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hjy");
MODULE_DESCRIPTION("DMA Engine memcpy test with coherent buffers");

module_platform_driver(dma_test_driver);
