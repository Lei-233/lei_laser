/*
 * galvo_hal.h - 振镜硬件抽象层接口
 *
 * 封装底层 ioctl 系统调用，为应用层提供简洁的 C 函数接口。
 * 应用程序不需要直接操作 /dev/galvo 设备节点和 ioctl 命令字。
 */

#ifndef _GALVO_HAL_H_
#define _GALVO_HAL_H_

#include "galvo_ioctl.h"

/* 设备生命周期 */
int galvo_open(void);
void galvo_close(void);

/* 核心批下发接口：一口气透传所有坐标进内核 vmalloc 池 */
int galvo_send_batch(struct galvo_point *points, uint32_t count);

/* 单点控制（遗留兼容接口） */
int galvo_set_ab(uint16_t a, uint16_t b);

/* 振镜回中复位（输出 2.5V，关激光） */
int galvo_home(void);

/* 使能/禁用驱动输出（en: 1=使能, 0=禁用） */
int galvo_set_enable(int en);

/* 查询批处理运行状态 */
int galvo_query_status(struct galvo_status *st);

#endif