// kernel/galvo_ioctl.c
#include <linux/uaccess.h>
#include <linux/errno.h>

#include "galvo_ioctl.h"

extern int galvo_apply_ab(uint16_t a, uint16_t b);

long galvo_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct galvo_ab ab;
    if (_IOC_TYPE(cmd) != GALVO_IOC_MAGIC)
        return -ENOTTY;

    switch (cmd) {
    case GALVO_IOC_SET_AB:
        if (copy_from_user(&ab, (void __user *)arg, sizeof(ab)))
            return -EFAULT;
        return galvo_apply_ab(ab.a, ab.b);

    default:
        return -ENOTTY;
    }
}
