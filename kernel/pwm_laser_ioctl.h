// pwm_laser_ioctl.h
#ifndef _PWM_LASER_IOCTL_H_
#define _PWM_LASER_IOCTL_H_

#include <linux/ioctl.h>
// #include <stdint.h>

#define PWM_LASER_IOC_MAGIC  's'

struct pwm_laser_target {
    uint16_t target;   // 0~1000
};

#define PWM_LASER_IOC_SET_TARGET  _IOW(PWM_LASER_IOC_MAGIC, 1, struct pwm_laser_target)
// 可选：读回当前值
#define PWM_LASER_IOC_GET_TARGET  _IOR(PWM_LASER_IOC_MAGIC, 2, struct pwm_laser_target)

#endif