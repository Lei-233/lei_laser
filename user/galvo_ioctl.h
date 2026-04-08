// user/galvo_ioctl.h
#ifndef _GALVO_IOCTL_H_
#define _GALVO_IOCTL_H_

#include <sys/ioctl.h>
#include <stdint.h>

struct galvo_point {
    uint16_t x;
    uint16_t y;
    uint16_t pwm_duty;
    uint32_t delay_us;
};

struct galvo_batch {
    struct galvo_point *points;
    uint32_t count;
};

#define GALVO_IOC_MAGIC 'g'
#define GALVO_IOC_SET_BATCH _IOW(GALVO_IOC_MAGIC, 4, struct galvo_batch)

#endif