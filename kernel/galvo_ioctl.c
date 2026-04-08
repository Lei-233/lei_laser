#include <linux/uaccess.h>
#include <linux/errno.h>

#include "galvo_ioctl.h"

extern int galvo_apply_ab(uint16_t a, uint16_t b);
extern int galvo_start_batch(struct galvo_point __user *user_points, uint32_t count);

long galvo_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct galvo_ab ab;
    struct galvo_batch batch;

    if (_IOC_TYPE(cmd) != GALVO_IOC_MAGIC)
        return -ENOTTY;

    switch (cmd) {
    case GALVO_IOC_SET_AB:
        if (copy_from_user(&ab, (void __user *)arg, sizeof(ab)))
            return -EFAULT;
        return galvo_apply_ab(ab.a, ab.b);

    case GALVO_IOC_SET_BATCH:
        if (copy_from_user(&batch, (void __user *)arg, sizeof(batch)))
            return -EFAULT;
        /* 一口血沉浇筑：彻底异步解绑 */
        return galvo_start_batch(batch.points, batch.count);

    default:
        return -ENOTTY;
    }
}
