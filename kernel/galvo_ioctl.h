// pwm_laser_ioctl.h
#ifndef _GALVO_IOCTL_H_
#define _GALVO_IOCTL_H_

#include <linux/ioctl.h>
// #include <stdint.h>

/* DAC8562SDGSR: 16位DAC，范围0-65535
 * a: X轴振镜控制值 (DAC-A)
 * b: Y轴振镜控制值 (DAC-B)
 * 输出电压: 0-5V (使用内部参考，增益=2)
 */
struct galvo_ab {
    uint16_t a;  /* 0-65535 -> 0-5V */
    uint16_t b;  /* 0-65535 -> 0-5V */
};

#define GALVO_IOC_MAGIC 'g'
#define GALVO_IOC_SET_AB  _IOW(GALVO_IOC_MAGIC, 1, struct galvo_ab)
#define GALVO_IOC_HOME    _IO(GALVO_IOC_MAGIC, 2)
#define GALVO_IOC_ENABLE _IOW(GALVO_IOC_MAGIC, 3, int)

#endif
