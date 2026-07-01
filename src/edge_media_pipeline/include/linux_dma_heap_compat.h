#pragma once

#include <linux/types.h>
#include <sys/ioctl.h>

// Some Orange Pi/RK3566 images ship older linux-libc-dev headers without
// linux/dma-heap.h even though /dev/dma_heap/system exists at runtime.
// Keep the kernel uapi definition local so the zero-copy DMABUF path still
// builds on those systems.
#ifndef DMA_HEAP_IOC_MAGIC
#define DMA_HEAP_IOC_MAGIC 'H'
#endif

#ifndef DMA_HEAP_IOCTL_ALLOC
struct dma_heap_allocation_data
{
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};

#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#endif
