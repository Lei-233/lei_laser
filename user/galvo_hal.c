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

void galvo_close(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}
