#ifndef _GALVO_IOCTL_H_
#define _GALVO_IOCTL_H_

#include <linux/ioctl.h>
#include <linux/types.h>

/* 单个点的打标属性 */
struct galvo_point {
    __u16 x;          /* X坐标，0-65535对应0-5V */
    __u16 y;          /* Y坐标，0-65535对应0-5V */
    __u16 pwm_duty;   /* 激光占空比，千分比: 0-1000 */
    __u32 delay_us;   /* 达到该点后停留/延时多少微秒 */
};

/* 批量传输结构体，用于vmalloc分配 */
struct galvo_batch {
    struct galvo_point __user *points; /* 指向用户态点位数组的指针 */
    __u32 count;                       /* 数组中点的数量 */
};

/* 遗留接口 */
struct galvo_ab {
    __u16 a;  /* 0-65535 -> 0-5V */
    __u16 b;  /* 0-65535 -> 0-5V */
};

#define GALVO_IOC_MAGIC 'g'
#define GALVO_IOC_SET_AB  _IOW(GALVO_IOC_MAGIC, 1, struct galvo_ab)
#define GALVO_IOC_HOME    _IO(GALVO_IOC_MAGIC, 2)
#define GALVO_IOC_ENABLE _IOW(GALVO_IOC_MAGIC, 3, int)
#define GALVO_IOC_SET_BATCH _IOW(GALVO_IOC_MAGIC, 4, struct galvo_batch)

#endif
