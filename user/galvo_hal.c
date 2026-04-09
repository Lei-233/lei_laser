/*
 * galvo_hal.c - 振镜硬件抽象层实现
 *
 * 封装 /dev/galvo 设备节点的 open/close/ioctl 操作，
 * 对上层屏蔽底层系统调用细节，提供类函数调用的简洁接口。
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <errno.h>

#include "galvo_hal.h"

static int g_fd = -1;

int galvo_open(void)
{
    if (g_fd >= 0)
        return g_fd;
    g_fd = open("/dev/galvo", O_RDWR);
    return g_fd;
}

/*
 * 核心批下发：构造 galvo_batch 封包头，通过唯一一次 ioctl 透传
 * ioctl 会在内核中触发 copy_from_user + vmalloc + hrtimer_start
 */
int galvo_send_batch(struct galvo_point *points, uint32_t count)
{
    struct galvo_batch batch;
    if (g_fd < 0) {
        errno = EBADF;
        return -1;
    }
    batch.points = points;
    batch.count = count;
    return ioctl(g_fd, GALVO_IOC_SET_BATCH, &batch);
}

/* 单点控制（遗留兼容） */
int galvo_set_ab(uint16_t a, uint16_t b)
{
    struct galvo_ab ab;
    if (g_fd < 0) {
        errno = EBADF;
        return -1;
    }
    ab.a = a;
    ab.b = b;
    return ioctl(g_fd, GALVO_IOC_SET_AB, &ab);
}

/* 振镜回中点复位 */
int galvo_home(void)
{
    if (g_fd < 0) {
        errno = EBADF;
        return -1;
    }
    return ioctl(g_fd, GALVO_IOC_HOME);
}

/* 使能/禁用 */
int galvo_set_enable(int en)
{
    if (g_fd < 0) {
        errno = EBADF;
        return -1;
    }
    return ioctl(g_fd, GALVO_IOC_ENABLE, &en);
}

/* 查询批处理状态 */
int galvo_query_status(struct galvo_status *st)
{
    if (g_fd < 0) {
        errno = EBADF;
        return -1;
    }
    return ioctl(g_fd, GALVO_IOC_QUERY_STATUS, st);
}

void galvo_close(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}
