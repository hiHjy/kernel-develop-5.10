/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Framework for buffer objects that can be shared across devices/subsystems.
 *
 * Copyright(C) 2015 Intel Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */


 /* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * 可在设备/子系统间共享的缓冲区对象框架。
 *
 * Copyright(C) 2015 Intel Ltd
 *
 * 本程序是自由软件；你可以根据自由软件基金会发布的 GNU 通用公共许可证
 * 第 2 版的条款重新分发和/或修改它。
 *
 * 本程序的分发基于这样的希望：它会有用，但**不提供任何担保**；
 * 甚至没有对适销性或特定用途适用性的暗示担保。
 * 详情请参见 GNU 通用公共许可证第 2 版。
 *
 * 你应该已经随本程序收到了一份 GNU 通用公共许可证第 2 版的副本。
 * 如果没有，请参见 <http://www.gnu.org/licenses/>。
 */
#ifndef _DMA_BUF_UAPI_H_
#define _DMA_BUF_UAPI_H_

#include <linux/types.h>

/* begin/end dma-buf functions used for userspace mmap. */
struct dma_buf_sync {
	__u64 flags;
};

#define DMA_BUF_SYNC_READ      (1 << 0)
#define DMA_BUF_SYNC_WRITE     (2 << 0)
#define DMA_BUF_SYNC_RW        (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START     (0 << 2)
#define DMA_BUF_SYNC_END       (1 << 2)
#define DMA_BUF_SYNC_VALID_FLAGS_MASK \
	(DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END)

#define DMA_BUF_NAME_LEN	32

#define DMA_BUF_BASE		'b'
#define DMA_BUF_IOCTL_SYNC	_IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)

/* 32/64bitness of this uapi was botched in android, there's no difference
 * between them in actual uapi, they're just different numbers.
 */
#define DMA_BUF_SET_NAME	_IOW(DMA_BUF_BASE, 1, const char *)
#define DMA_BUF_SET_NAME_A	_IOW(DMA_BUF_BASE, 1, __u32)
#define DMA_BUF_SET_NAME_B	_IOW(DMA_BUF_BASE, 1, __u64)

struct dma_buf_sync_partial {
	__u64 flags;
	__u32 offset;
	__u32 len;
};

#define DMA_BUF_IOCTL_SYNC_PARTIAL	_IOW(DMA_BUF_BASE, 2, struct dma_buf_sync_partial)

#endif
