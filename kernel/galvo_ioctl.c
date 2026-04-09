/*
 * galvo_ioctl.c - 振镜设备 ioctl 命令分发处理
 *
 * 完整实现了所有 5 个自定义 ioctl 命令：
 *   SET_AB       - 遗留接口：直接设置 DAC 双通道电压（兼容单点 G-code 控制）
 *   HOME         - 振镜回中点复位（输出 2.5V，关激光）
 *   ENABLE       - 使能/禁用驱动（禁用时拒绝新任务）
 *   SET_BATCH    - 核心接口：批量坐标一口气下沉进内核 vmalloc 池
 *   QUERY_STATUS - 查询当前批处理运行状态（点位进度 + SPI 错误计数）
 *
 * 为什么用 ioctl 而不用 write()？
 *   write() 只能排队塞字节流，分不清指令和配置。
 *   ioctl 天生是带"命令字暗号"的分包通道，内核瞬间领会意图并接管结构体指针。
 *   对于包含 X/Y/延时/功率等多属性的复杂结构体控制，非 ioctl 不可。
 */

#include <linux/uaccess.h>
#include <linux/errno.h>

#include "galvo_ioctl.h"

extern int galvo_apply_ab(uint16_t a, uint16_t b);
extern int galvo_start_batch(struct galvo_point __user *user_points, uint32_t count);
extern int galvo_home(void);
extern int galvo_set_enable(int en);
extern int galvo_get_status(struct galvo_status *st);

long galvo_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct galvo_ab ab;
    struct galvo_batch batch;
    struct galvo_status status;
    int enable;

    /* Magic Number 校验：过滤掉非本驱动的误传命令 */
    if (_IOC_TYPE(cmd) != GALVO_IOC_MAGIC)
        return -ENOTTY;

    switch (cmd) {
    /* 遗留单点控制接口（兼容早期 G-code 控制器） */
    case GALVO_IOC_SET_AB:
        if (copy_from_user(&ab, (void __user *)arg, sizeof(ab)))
            return -EFAULT;
        return galvo_apply_ab(ab.a, ab.b);

    /* 振镜回中点复位 */
    case GALVO_IOC_HOME:
        return galvo_home();

    /* 使能/禁用驱动输出 */
    case GALVO_IOC_ENABLE:
        if (copy_from_user(&enable, (void __user *)arg, sizeof(enable)))
            return -EFAULT;
        return galvo_set_enable(enable);

    /*
     * 核心批下发接口
     *
     * 应用层构造封包指针头，携带 GALVO_IOC_SET_BATCH 特权宏命令字，
     * 通过仅此一次 ioctl 将千万点位安全下沉进内核 vmalloc 缓冲河道。
     * ioctl 瞬时返回宣告收工，剩下搬运打点的流水线微米级苦差，
     * 交由 hrtimer 结合硬件定时平滑代劳。
     */
    case GALVO_IOC_SET_BATCH:
        if (copy_from_user(&batch, (void __user *)arg, sizeof(batch)))
            return -EFAULT;
        return galvo_start_batch(batch.points, batch.count);

    /*
     * 状态查询接口
     * 返回：运行状态(IDLE/RUNNING)、总点数、已完成点数、SPI 累计错误数
     * 方向：内核 → 用户空间（_IOR）
     */
    case GALVO_IOC_QUERY_STATUS:
        galvo_get_status(&status);
        if (copy_to_user((void __user *)arg, &status, sizeof(status)))
            return -EFAULT;
        return 0;

    default:
        return -ENOTTY;
    }
}
