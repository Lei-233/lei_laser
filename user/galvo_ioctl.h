/*
 * user/galvo_ioctl.h - 振镜驱动 ioctl 接口定义（用户空间版本）
 *
 * 此文件是内核 kernel/galvo_ioctl.h 的用户态镜像，
 * 结构体布局和命令字必须与内核版本严格一一对应。
 *
 * 区别：
 *   - 内核版本使用 <linux/ioctl.h> 和 __u16/__u32 类型
 *   - 用户版本使用 <sys/ioctl.h> 和 uint16_t/uint32_t 类型
 */

#ifndef _GALVO_IOCTL_H_
#define _GALVO_IOCTL_H_

#include <sys/ioctl.h>
#include <stdint.h>

/* 单个打标点位属性（与内核 struct galvo_point 布局完全对应） */
struct galvo_point {
    uint16_t x;          /* X 轴 DAC 值，0-65535 对应 0-5V */
    uint16_t y;          /* Y 轴 DAC 值，0-65535 对应 0-5V */
    uint16_t pwm_duty;   /* 激光占空比，千分比: 0-1000 */
    uint32_t delay_us;   /* 停驻延时（微秒） */
};

/* 批量传输封包头（通过唯一一次 ioctl 透传进内核） */
struct galvo_batch {
    struct galvo_point *points;  /* 指向用户堆内存中的点位数组 */
    uint32_t count;              /* 点位数量 */
};

/* 单点控制接口（兼容遗留 G-code 控制器） */
struct galvo_ab {
    uint16_t a;  /* DAC-A: 0-65535 -> 0-5V */
    uint16_t b;  /* DAC-B: 0-65535 -> 0-5V */
};

/* 批处理运行状态查询 */
struct galvo_status {
    uint32_t state;            /* 0=空闲(IDLE), 1=运行中(RUNNING) */
    uint32_t total_points;     /* 当前批次总点数 */
    uint32_t completed_points; /* 已完成点数 */
    uint32_t spi_errors;       /* 累计 SPI 传输错误计数 */
};

/* ioctl 命令字（必须与内核 galvo_ioctl.h 严格一致） */
#define GALVO_IOC_MAGIC 'g'

#define GALVO_IOC_SET_AB       _IOW(GALVO_IOC_MAGIC, 1, struct galvo_ab)
#define GALVO_IOC_HOME         _IO(GALVO_IOC_MAGIC, 2)
#define GALVO_IOC_ENABLE       _IOW(GALVO_IOC_MAGIC, 3, int)
#define GALVO_IOC_SET_BATCH    _IOW(GALVO_IOC_MAGIC, 4, struct galvo_batch)
#define GALVO_IOC_QUERY_STATUS _IOR(GALVO_IOC_MAGIC, 5, struct galvo_status)

#endif