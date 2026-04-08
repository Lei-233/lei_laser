#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>

#include "pwm_laser_hal.h"
#include "pwm_laser_ioctl.h"

static int g_fd = -1;

int pwm_laser_open(void)
{
    g_fd = open("/dev/pwm_laser", O_RDWR);
    return g_fd;
}

int pwm_laser_set_target(int target)
{
    if (g_fd < 0) return -1;

    if (target < 0) target = 0;
    if (target > 1000) target = 1000;

    struct pwm_laser_target arg;
    arg.target = (uint16_t)target;

    return ioctl(g_fd, PWM_LASER_IOC_SET_TARGET, &arg);
}

void pwm_laser_close(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}
