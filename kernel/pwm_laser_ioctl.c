// kernel/pwm_laser_ioctl.c
#include <linux/uaccess.h>
#include <linux/errno.h>

#include "pwm_laser_ioctl.h"

// 来自 pwm_laser_drv.c
extern int pwm_laser_apply_target(uint16_t percentage);

long pwm_laser_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct pwm_laser_target data;

    if (_IOC_TYPE(cmd) != PWM_LASER_IOC_MAGIC)
        return -ENOTTY;

    switch (cmd) {
    case PWM_LASER_IOC_SET_TARGET:
        if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
            return -EFAULT;

        pwm_laser_apply_target(data.target);
        return 0;

    case PWM_LASER_IOC_GET_TARGET:
        // 可选：如果你想读回当前 target，就需要在 drv.c 里保存 current_target
        // data.target = current_target;
        // if (copy_to_user((void __user *)arg, &data, sizeof(data)))
        //     return -EFAULT;
        return -ENOTTY;

    default:
        return -ENOTTY;
    }
}
