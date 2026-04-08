#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <errno.h>

#include "galvo_hal.h"
#include "galvo_ioctl.h"

static int g_fd = -1;

int galvo_open(void)
{
    if (g_fd >= 0)
        return g_fd;

    g_fd = open("/dev/galvo", O_RDWR);
    return g_fd;
}

/* DAC8562: 16位范围 0-65535 */
static inline uint16_t clamp_uint16_t(uint16_t v)
{
    /* uint16_t已经是0-65535范围，无需clamp */
    return v;
}

int galvo_set_ab(uint16_t a, uint16_t b)
{
    struct galvo_ab ab;

    if (g_fd < 0) {
        errno = EBADF;
        return -1;
    }

    ab.a = clamp_uint16_t(a);
    ab.b = clamp_uint16_t(b);

    return ioctl(g_fd, GALVO_IOC_SET_AB, &ab);
}

void galvo_close(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}
