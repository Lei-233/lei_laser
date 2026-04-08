// pwm_laser_ioctl.h
#ifndef _GALVO_IOCTL_H_
#define _GALVO_IOCTL_H_

#include <sys/ioctl.h>
#include <stdint.h>

struct galvo_ab {
    uint16_t a;
    uint16_t b;
};

#define GALVO_IOC_MAGIC 'g'
#define GALVO_IOC_SET_AB  _IOW(GALVO_IOC_MAGIC, 1, struct galvo_ab)
#define GALVO_IOC_HOME    _IO(GALVO_IOC_MAGIC, 2)
#define GALVO_IOC_ENABLE _IOW(GALVO_IOC_MAGIC, 3, int)

#endif