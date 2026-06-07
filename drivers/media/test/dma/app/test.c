#include "stdio.h"
#include "sys/ioctl.h"
#include "stdint.h"
#include "unistd.h"
#include <fcntl.h>
#include "sys/mman.h"
#include <errno.h> 
#include "string.h"

#define TEST_SIZE 4096
#define DUMP_SIZE 100

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
	uint32_t size;
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
	uint32_t size;
	uint32_t src_mmap_offset;
	uint32_t dst_mmap_offset;
};

static void dump_buffer(const char *name, const void *buf, size_t size)
{
	const unsigned char *p = buf;
	size_t dump_size = size < DUMP_SIZE ? size : DUMP_SIZE;
	size_t i;

	for (i = 0; i < dump_size; i++) {
		if (i % 16 == 0)
			printf("%s %08zx: ", name, i);

		printf("%02x ", p[i]);

		if (i % 16 == 15 || i == dump_size - 1)
			printf("\n");
	}
}

int main(void) {
    struct dma_test_info_arg info;
    int fd = open("/dev/dma_chardev_template", O_RDWR);
    if (fd < 0) {
        perror("open");
        return -1;
    }
    struct dma_test_alloc_arg arg = {
        .size = TEST_SIZE
    };
    int ret = ioctl(fd, DMA_TEST_IOC_ALLOC, &arg);
    if (ret < 0) {
        fprintf(stderr, " ioctl(fd, DMA_TEST_IOC_ALLOC, &arg)\n");
        return -1;
    }

    ret = ioctl(fd, DMA_TEST_IOC_INFO, &info);
    if (ret < 0) {
        fprintf(stderr, " ioctl(fd, DMA_TEST_IOC_INFO, &info)\n");
        return -1;
    }

    void *src = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                     info.src_mmap_offset);
    if (src == MAP_FAILED) {
        perror("mmap failed");
        // 获取详细错误码
        int err = errno;
        printf("mmap error: %d (%s)\n", err, strerror(err));
        return -1;
    }
    printf("user space src_cpu:%p\n", src);
    void *dst = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                     info.dst_mmap_offset);
    if (dst == MAP_FAILED) {
        perror("mmap failed");
        // 获取详细错误码
        int err = errno;
        printf("mmap error: %d (%s)\n", err, strerror(err));
        return -1;
    }
    printf("user space dst_cpu:%p\n", dst);


    printf("应用层在操作用户空间指针前buf前100字节内容:\n");
    dump_buffer("src", src, info.size);
    dump_buffer("dst", dst, info.size);

    printf("DMA内容:\n");
    char buf[1];

    read(fd, &buf, sizeof(buf));

    memset(src, 0x5a, info.size);
    memset(dst, 0x55, info.size);

    printf("应用层在操作用户空间指针后buf前100字节内容:\n");
    dump_buffer("src", src, info.size);
    dump_buffer("dst", dst, info.size);
    printf("DMA内容:\n");

    read(fd, &buf, sizeof(buf));
    

    munmap(src, info.size);
    munmap(dst, info.size);

    ret = ioctl(fd, DMA_TEST_IOC_FREE);
    if (ret < 0) {
        fprintf(stderr, " ioctl(fd, DMA_TEST_IOC_ALLOC, &arg)\n");
        return -1;
    }


    return 0;
}
