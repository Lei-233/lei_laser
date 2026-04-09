/*
 * galvo_ioctl.h - 振镜驱动 ioctl 接口定义（内核空间版本）
 *
 * 本文件定义了用户空间与内核驱动之间的通信协议：
 *   - 数据结构体（galvo_point / galvo_batch / galvo_ab / galvo_status）
 *   - ioctl 命令字（基于 _IOW/_IOR/_IO 宏，携带 Magic Number 'g'）
 *
 * 用户空间有一份对应的用户态版本（使用 <sys/ioctl.h> 和 <stdint.h>），
 * 两份头文件的结构体布局和命令字必须严格一一对应。
 */

#ifndef _GALVO_IOCTL_H_
#define _GALVO_IOCTL_H_

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * 单个打标点位的完整属性结构体
 *
 * 每个结构体封装了振镜偏转坐标、激光脉冲强度和停驻时间，
 * 这些强关联属性必须在同一个中断帧内被原子化地推送到硬件。
 * 这也是为什么不能用简单的 write() 字节流，而必须用 ioctl 结构体通道的根本原因。
 */
struct galvo_point {
    __u16 x;          /* X 轴 DAC 值，0-65535 对应 0-5V 振镜偏转电压 */
    __u16 y;          /* Y 轴 DAC 值，0-65535 对应 0-5V 振镜偏转电压 */
    __u16 pwm_duty;   /* 激光占空比，千分比: 0=关闭, 1000=全功率 */
    __u32 delay_us;   /* 到达该点后的停驻延时（微秒），由 hrtimer 精确执行 */
};

/*
 * 批量传输封包头结构体
 *
 * 应用层在用户态堆内存中一口气计算完所有轨迹坐标后，
 * 将指针和计数封入此结构体，通过唯一一次 ioctl 系统调用透传给内核。
 * 内核侧使用 copy_from_user() 安全搬运数据进 vmalloc 大缓冲池。
 */
struct galvo_batch {
    struct galvo_point __user *points; /* 指向用户态点位数组的指针（不可在内核直接解引用！） */
    __u32 count;                       /* 数组中点的数量 */
};

/* 遗留单点控制接口（兼容 G-code 控制器的逐点模式） */
struct galvo_ab {
    __u16 a;  /* DAC-A 通道值, 0-65535 -> 0-5V */
    __u16 b;  /* DAC-B 通道值, 0-65535 -> 0-5V */
};

/*
 * 批处理运行状态查询结构体
 *
 * 应用层可通过 GALVO_IOC_QUERY_STATUS 实时获取内核批处理进度，
 * 用于 UI 进度条显示或决定是否安全下发下一批任务。
 */
struct galvo_status {
    __u32 state;            /* 0 = 空闲(IDLE)：hrtimer 已停止, 1 = 运行中(RUNNING) */
    __u32 total_points;     /* 当前批次总点数 */
    __u32 completed_points; /* 已完成点数（由 hrtimer 回调递增） */
    __u32 spi_errors;       /* 累计 SPI 异步传输错误计数（由完成回调递增） */
};

/* ioctl 命令字定义 */
#define GALVO_IOC_MAGIC 'g'

#define GALVO_IOC_SET_AB       _IOW(GALVO_IOC_MAGIC, 1, struct galvo_ab)     /* 单点控制 */
#define GALVO_IOC_HOME         _IO(GALVO_IOC_MAGIC, 2)                       /* 振镜回中复位 */
#define GALVO_IOC_ENABLE       _IOW(GALVO_IOC_MAGIC, 3, int)                 /* 使能/禁用 */
#define GALVO_IOC_SET_BATCH    _IOW(GALVO_IOC_MAGIC, 4, struct galvo_batch)  /* 批量下发 */
#define GALVO_IOC_QUERY_STATUS _IOR(GALVO_IOC_MAGIC, 5, struct galvo_status) /* 状态查询 */

#endif
