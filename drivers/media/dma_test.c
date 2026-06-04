#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#define DEVICE_NAME "dma_test"
#define DMA_SIZE 4096

struct test_dev {
	struct device *dev;
	void *cpu_addr;
	dma_addr_t dma_addr;
};

static const struct of_device_id dma_test_of_match[] = {
	{ .compatible = "hjy,dma_test" },
	{}
};

static int dma_test_probe(struct platform_device *pdev)
{
	struct test_dev *tdev;
	/*
    参数：

        &pdev->dev
        当前设备对象
        devm 管理的资源会在设备卸载时自动释放
        sizeof(*tdev)
        申请结构体大小
        推荐这种写法，后续修改类型不用改大小
        GFP_KERNEL
        普通内核内存申请标志
    */

	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	tdev->dev = &pdev->dev;

	platform_set_drvdata(pdev, tdev);
    
    tdev->cpu_addr = dma_alloc_coherent(tdev->dev, DMA_SIZE, &tdev->dma_addr, GFP_KERNEL);
    if (!tdev->cpu_addr) {
        return -ENOMEM;
    }

    // memset(tdev->cpu_addr, 0x5a, DMA_SIZE);
	dev_info(tdev->dev, "cpu_addr=%p dma_addr=%pad size=%d\n",
		 tdev->cpu_addr, &tdev->dma_addr, DMA_SIZE);
	dev_info(tdev->dev, "dma_test_probe success\n");
	return 0;
}

static int dma_test_remove(struct platform_device *pdev)
{
    struct test_dev* tdev = platform_get_drvdata(pdev);
    dma_free_coherent(tdev->dev, DMA_SIZE, tdev->cpu_addr, tdev->dma_addr);
	return 0;
}
static struct platform_driver dma_test_driver = {
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = dma_test_of_match,
	},
	.probe =  dma_test_probe,
	.remove = dma_test_remove,
};
MODULE_DEVICE_TABLE(of, dma_test_of_match);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("hjy");
MODULE_DESCRIPTION("DMA coherent allocation test");
module_platform_driver(dma_test_driver);
