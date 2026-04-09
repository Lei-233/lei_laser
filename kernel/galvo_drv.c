/*
 * galvo_drv.c - 工业级高性能激光打标振镜驱动
 *
 * 核心架构：hrtimer 高精度定时器 + spi_async 异步非阻塞通信 + PWM 底层同步控制
 *
 * 设计目标：
 *   在非实时的 Linux 系统中，利用 hrtimer 硬中断回调强行辟出一条微秒级硬实时快车道。
 *   通过 vmalloc 大缓冲池接纳应用层一次性下发的万级坐标结构体，
 *   再由内核定时器异步逐点驱动 DAC8562 振镜与激光 PWM 的绝对动作同步。
 *
 * 关键技术：
 *   - 弃用低效 spidev 通用节点，手撕专属字符设备 galvo.ko
 *   - 自定义 ioctl 接口：SET_BATCH / SET_AB / HOME / ENABLE / QUERY_STATUS
 *   - vmalloc 分配（非 kmalloc）：规避长时间运行后物理内存碎片化导致的分配失败
 *   - spi_async 异步提交：中断上下文绝不休眠，完美符合"干完就跑"的安全死线
 *   - SPI 双缓冲乒乓切换：杜绝 DMA 传输与 hrtimer 回调之间的缓冲区竞态
 *   - spinlock 并发保护：保障批处理状态在中断与进程上下文间的一致性
 *
 * 硬件平台：RK3566 (泰山派, Buildroot Linux)
 * DAC 芯片：DAC8562SDGSR (TI, 16 位双通道, SPI Mode 1)
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/vmalloc.h>
#include <linux/pwm.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>

#include "galvo_ioctl.h"

int galvo_driver_init(void);
void galvo_driver_exit(void);

dev_t dev_num;
struct cdev galvo_cdev;
struct class *galvo_class;
struct device *galvo_device;

static struct spi_device *galvo_spi_device;

static struct pwm_device *galvo_pwm_dev;

/* 超高精度内核驱动定时器：直接绑定底层硬件计数器，支持纳秒级 ktime_t */
static struct hrtimer galvo_timer;

/* vmalloc 大缓冲池：突破 kmalloc 物理连续内存的碎片化束缚 */
static struct galvo_point *batch_buffer = NULL;
static uint32_t batch_count = 0;
static uint32_t current_point_idx = 0;

/* 使能控制 */
static int galvo_enabled = 1;

/*
 * SPI 双缓冲乒乓切换机制
 *
 * spi_async() 调用后函数立即返回（"干完就跑"），但底层 DMA 引擎仍在访问 tx_buf。
 * 如果 hrtimer 下一次回调覆写了同一个缓冲区，就会造成数据竞态——
 * DMA 搬运到一半的数据被修改，导致 DAC 收到错误的电压值。
 *
 * 解决方案：分配两组完全独立的 SPI 缓冲区，每次 hrtimer 回调切换到另一组。
 * 当第 N 帧使用 buf[0] 时，第 N-1 帧的 buf[1] 上的 DMA 已安全完成。
 */
#define SPI_BUF_COUNT 2
static struct {
    struct spi_message msg_a;
    struct spi_message msg_b;
    struct spi_transfer xfer_a;
    struct spi_transfer xfer_b;
    uint8_t tx_buf[6];
} spi_buf[SPI_BUF_COUNT];
static int spi_buf_idx = 0;

/*
 * SPI 异步传输错误追踪
 * 使用原子变量，因为完成回调可能在任意中断上下文中被调用
 */
static atomic_t spi_error_count = ATOMIC_INIT(0);

/*
 * spinlock 并发保护
 * 保护 batch_buffer / batch_count / current_point_idx 共享状态，
 * 防止 hrtimer 中断回调与应用层 ioctl 进程上下文同时操作导致的竞态
 */
static DEFINE_SPINLOCK(galvo_lock);

// ioctl 处理函数在 galvo_ioctl.c 里实现
extern long galvo_ioctl(struct file *, unsigned int, unsigned long);

/* DAC8562 命令定义（24位帧格式：[CMD 8bit][DATA 16bit]） */
#define DAC8562_CMD_WRITE_A_UPDATE   0x18  /* 写 DAC-A 并立即更新输出 */
#define DAC8562_CMD_WRITE_B_UPDATE   0x19  /* 写 DAC-B 并立即更新输出 */
#define DAC8562_CMD_WRITE_AB_UPDATE  0x1F  /* 同时写 DAC-AB 并更新 */
#define DAC8562_CMD_POWERUP          0x20  /* 上电控制 */
#define DAC8562_CMD_RESET            0x28  /* 软件复位 */
#define DAC8562_CMD_INTERNAL_REF     0x38  /* 内部参考电压使能（增益=2, 输出 0-5V） */

#define DAC8562_DATA_POWERUP_AB      0x0003
#define DAC8562_DATA_RESET           0x0001
#define DAC8562_DATA_REF_ENABLE      0x0001

static inline uint16_t clamp16(uint16_t v) { return v; }

/* 遗留的同步发送方式（用于初始化和单点控制，非高频路径） */
static int dac8562_write(uint8_t cmd, uint16_t data)
{
    uint8_t tx[3];
    if (!galvo_spi_device) return -ENODEV;
    tx[0] = cmd; tx[1] = (data >> 8) & 0xFF; tx[2] = data & 0xFF;
    return spi_write(galvo_spi_device, tx, 3);
}

int galvo_apply_ab(uint16_t a, uint16_t b)
{
    int ret;
    if (!galvo_spi_device) return -ENODEV;
    ret = dac8562_write(DAC8562_CMD_WRITE_A_UPDATE, clamp16(a));
    if (ret < 0) return ret;
    ret = dac8562_write(DAC8562_CMD_WRITE_B_UPDATE, clamp16(b));
    return ret;
}

static int dac8562_init(void)
{
    int ret;
    /* 初始化序列：复位 → 使能内部参考 → 上电 → 设中点 */
    ret = dac8562_write(DAC8562_CMD_RESET, DAC8562_DATA_RESET);
    if (ret < 0) return ret;
    udelay(10);
    ret = dac8562_write(DAC8562_CMD_INTERNAL_REF, DAC8562_DATA_REF_ENABLE);
    if (ret < 0) return ret;
    udelay(10);
    ret = dac8562_write(DAC8562_CMD_POWERUP, DAC8562_DATA_POWERUP_AB);
    if (ret < 0) return ret;
    udelay(10);
    /* 输出中点电压 2.5V（DAC值 32768 = 0x8000） */
    ret = dac8562_write(DAC8562_CMD_WRITE_A_UPDATE, 32768);
    if (ret < 0) return ret;
    ret = dac8562_write(DAC8562_CMD_WRITE_B_UPDATE, 32768);
    return ret;
}

/*
 * spi_async 异步完成回调
 *
 * 当底层 SPI 控制器（或 DMA）真正完成物理传输后，此回调在中断上下文中被调用。
 * 我们在这里检查传输状态，如果出错则累加原子错误计数器。
 * 注意：此回调仅做极轻量的检查，绝不休眠，符合中断上下文"干完就跑"的铁律。
 */
static void spi_complete_callback(void *context)
{
    struct spi_message *msg = (struct spi_message *)context;
    if (msg->status != 0) {
        atomic_inc(&spi_error_count);
        pr_warn("galvo: spi_async transfer error, status=%d\n", msg->status);
    }
}

/*
 * 高精度定时器回调 —— 整套系统的心脏
 *
 * 运行在硬件中断上下文中（不受 Linux 进程调度器干扰），
 * 直接绑定 RK3566 底层硬件计数器，实现微秒级精确步进。
 *
 * 关键约束（中断上下文铁律）：
 *   ❌ 绝对不能有任何休眠/阻塞（不能用 spi_sync、mutex、msleep 等）
 *   ✅ 必须使用 spi_async 异步接口（"嗖地丢进硬件队列，瞬间返回脱身"）
 *   ✅ 必须使用 spin_lock（不能用 mutex，mutex 会触发调度器）
 */
static enum hrtimer_restart galvo_timer_callback(struct hrtimer *timer)
{
    struct galvo_point *pt;
    uint32_t duty_ns;
    unsigned long flags;
    int buf_cur;

    spin_lock_irqsave(&galvo_lock, flags);

    if (current_point_idx >= batch_count || !batch_buffer) {
        spin_unlock_irqrestore(&galvo_lock, flags);
        if (galvo_pwm_dev) pwm_config(galvo_pwm_dev, 0, 1000000);
        return HRTIMER_NORESTART;
    }

    pt = &batch_buffer[current_point_idx];

    /* 1. 同步进行 PWM 更新：振镜偏转与激光通断在同一中断帧内对齐 */
    duty_ns = (pt->pwm_duty > 1000 ? 1000 : pt->pwm_duty) * 1000;
    if (galvo_pwm_dev) {
        pwm_config(galvo_pwm_dev, duty_ns, 1000000);
    }

    /* 2. 乒乓切换 SPI 缓冲区，杜绝 DMA 竞态 */
    buf_cur = spi_buf_idx;
    spi_buf_idx = (spi_buf_idx + 1) % SPI_BUF_COUNT;

    /* 3. 填充当前缓冲区的发送数据 */
    spi_buf[buf_cur].tx_buf[0] = DAC8562_CMD_WRITE_A_UPDATE;
    spi_buf[buf_cur].tx_buf[1] = (pt->x >> 8) & 0xFF;
    spi_buf[buf_cur].tx_buf[2] = pt->x & 0xFF;

    spi_buf[buf_cur].tx_buf[3] = DAC8562_CMD_WRITE_B_UPDATE;
    spi_buf[buf_cur].tx_buf[4] = (pt->y >> 8) & 0xFF;
    spi_buf[buf_cur].tx_buf[5] = pt->y & 0xFF;

    current_point_idx++;

    spin_unlock_irqrestore(&galvo_lock, flags);

    /* 4. 纯异步非阻塞提交 SPI 任务（在锁外执行，避免持锁时间过长） */
    if (galvo_spi_device) {
        spi_async(galvo_spi_device, &spi_buf[buf_cur].msg_a);
        spi_async(galvo_spi_device, &spi_buf[buf_cur].msg_b);
    }

    if (current_point_idx >= batch_count) {
        if (galvo_pwm_dev) pwm_config(galvo_pwm_dev, 0, 1000000); /* 运行完毕后关光 */
        return HRTIMER_NORESTART;
    }

    /* 将定时器推向下一个延时节点，实现微秒级精确步进 */
    hrtimer_forward_now(timer, ns_to_ktime(pt->delay_us * 1000ULL));
    return HRTIMER_RESTART;
}

/*
 * 提供给 ioctl 使用的批下发通道
 *
 * 应用层通过唯一一次 ioctl(GALVO_IOC_SET_BATCH) 系统调用，
 * 将万级坐标结构体安全下沉进内核 vmalloc 缓冲池，
 * 然后 hrtimer 自动接管后续的微秒级逐点驱动。
 *
 * 这就是面试中阐述的"应用层统筹算力 → 唯一一次系统调用透穿 → 彻底解绑脱身"架构。
 */
int galvo_start_batch(struct galvo_point __user *user_points, uint32_t count)
{
    unsigned long flags;

    if (count == 0) return 0;
    if (!galvo_enabled) return -EPERM;

    hrtimer_cancel(&galvo_timer);

    spin_lock_irqsave(&galvo_lock, flags);

    if (batch_buffer) {
        vfree(batch_buffer);
        batch_buffer = NULL;
    }

    /* vmalloc 分配：虚拟地址连续，物理页可以东拼西凑，斩断碎片化隐患 */
    batch_buffer = vmalloc(count * sizeof(struct galvo_point));
    if (!batch_buffer) {
        spin_unlock_irqrestore(&galvo_lock, flags);
        pr_err("galvo: failed to vmalloc %u points (%lu bytes)\n",
               count, (unsigned long)(count * sizeof(struct galvo_point)));
        return -ENOMEM;
    }

    spin_unlock_irqrestore(&galvo_lock, flags);

    /*
     * copy_from_user —— 内核驱动的安全绝对铁律
     *
     * 绝不可直接解引用用户态指针（会引发 Kernel Panic 死机）。
     * 必须通过此 "带安检的系统级复印机" 将用户态数据安全搬运进内核私有领地。
     */
    if (copy_from_user(batch_buffer, user_points, count * sizeof(struct galvo_point))) {
        spin_lock_irqsave(&galvo_lock, flags);
        vfree(batch_buffer);
        batch_buffer = NULL;
        spin_unlock_irqrestore(&galvo_lock, flags);
        return -EFAULT;
    }

    spin_lock_irqsave(&galvo_lock, flags);
    batch_count = count;
    current_point_idx = 0;
    spin_unlock_irqrestore(&galvo_lock, flags);

    if (galvo_pwm_dev) {
        pwm_enable(galvo_pwm_dev);
    }

    /* 第一帧直接由定时器驱动起飞 */
    hrtimer_start(&galvo_timer, ktime_set(0, 0), HRTIMER_MODE_REL);

    return 0;
}

/*
 * HOME 复位：振镜回中点（2.5V），停止定时器，关闭激光
 */
int galvo_home(void)
{
    unsigned long flags;

    hrtimer_cancel(&galvo_timer);

    spin_lock_irqsave(&galvo_lock, flags);
    current_point_idx = batch_count; /* 标记批处理完成 */
    spin_unlock_irqrestore(&galvo_lock, flags);

    if (galvo_pwm_dev) pwm_config(galvo_pwm_dev, 0, 1000000);

    /* DAC 输出中点 32768 = 2.5V */
    return galvo_apply_ab(32768, 32768);
}

/*
 * ENABLE 使能控制：禁用时停止一切输出并拒绝新的批处理任务
 */
int galvo_set_enable(int en)
{
    galvo_enabled = en;
    if (!en) {
        hrtimer_cancel(&galvo_timer);
        if (galvo_pwm_dev) pwm_config(galvo_pwm_dev, 0, 1000000);
    }
    return 0;
}

/*
 * 状态查询：返回当前批处理的运行状态快照
 * 使用 spin_lock_irqsave 确保读取到的是一致性快照
 */
int galvo_get_status(struct galvo_status *st)
{
    unsigned long flags;

    spin_lock_irqsave(&galvo_lock, flags);
    st->state = (current_point_idx < batch_count && batch_buffer) ? 1 : 0;
    st->total_points = batch_count;
    st->completed_points = current_point_idx;
    spin_unlock_irqrestore(&galvo_lock, flags);

    st->spi_errors = (uint32_t)atomic_read(&spi_error_count);
    return 0;
}


static int galvo_open(struct inode *inode, struct file *file) { return 0; }
static int galvo_release(struct inode *inode, struct file *file) { return 0; }
static ssize_t galvo_read(struct file *file, char __user *buf, size_t len, loff_t *off) { return 0; }
static ssize_t galvo_write(struct file *f, const char __user *buf, size_t len, loff_t *off) { return -EILSEQ; }

struct file_operations galvo_fops = {
    .owner          = THIS_MODULE,
    .open           = galvo_open,
    .write          = galvo_write,
    .release        = galvo_release,
    .read           = galvo_read,
    .unlocked_ioctl = galvo_ioctl,
};

int galvo_probe(struct spi_device *spi)
{
    int ret, i;

    galvo_spi_device = spi;
    spi_set_drvdata(spi, galvo_spi_device);

    /* SPI Mode 1 (CPOL=0, CPHA=1)：DAC8562 数据手册要求 */
    spi->mode = SPI_MODE_1;
    spi->bits_per_word = 8;
    if (!spi->max_speed_hz) spi->max_speed_hz = 20000000;

    ret = spi_setup(spi);
    if (ret) return ret;

    /* 初始化双缓冲 SPI 异步消息结构 */
    for (i = 0; i < SPI_BUF_COUNT; i++) {
        memset(&spi_buf[i], 0, sizeof(spi_buf[i]));

        spi_message_init(&spi_buf[i].msg_a);
        spi_buf[i].xfer_a.tx_buf = &spi_buf[i].tx_buf[0];
        spi_buf[i].xfer_a.len = 3;
        spi_message_add_tail(&spi_buf[i].xfer_a, &spi_buf[i].msg_a);
        spi_buf[i].msg_a.complete = spi_complete_callback;
        spi_buf[i].msg_a.context = &spi_buf[i].msg_a;

        spi_message_init(&spi_buf[i].msg_b);
        spi_buf[i].xfer_b.tx_buf = &spi_buf[i].tx_buf[3];
        spi_buf[i].xfer_b.len = 3;
        spi_message_add_tail(&spi_buf[i].xfer_b, &spi_buf[i].msg_b);
        spi_buf[i].msg_b.complete = spi_complete_callback;
        spi_buf[i].msg_b.context = &spi_buf[i].msg_b;
    }

    /* 获取 PWM 节点，实现激光通断与振镜偏转的底层绝对同步 */
    galvo_pwm_dev = devm_pwm_get(&spi->dev, NULL);
    if (IS_ERR(galvo_pwm_dev)) {
        galvo_pwm_dev = NULL;
    } else {
        pwm_config(galvo_pwm_dev, 0, 1000000);
        pwm_set_polarity(galvo_pwm_dev, PWM_POLARITY_NORMAL);
    }

    /* 建立超高精度内核驱动定时器（直接绑定硬件计数器，非 jiffies 节拍驱动） */
    hrtimer_init(&galvo_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    galvo_timer.function = galvo_timer_callback;

    dac8562_init();

    /* 注册字符设备（使用 goto 错误回退链保证资源安全释放） */
    ret = alloc_chrdev_region(&dev_num, 0, 1, "galvo");
    if (ret < 0) {
        pr_err("galvo: alloc_chrdev_region failed\n");
        return ret;
    }

    cdev_init(&galvo_cdev, &galvo_fops);
    ret = cdev_add(&galvo_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("galvo: cdev_add failed\n");
        goto err_cdev;
    }

    galvo_class = class_create(THIS_MODULE, "galvo_class");
    if (IS_ERR(galvo_class)) {
        ret = PTR_ERR(galvo_class);
        pr_err("galvo: class_create failed\n");
        goto err_class;
    }

    galvo_device = device_create(galvo_class, NULL, dev_num, NULL, "galvo");
    if (IS_ERR(galvo_device)) {
        ret = PTR_ERR(galvo_device);
        pr_err("galvo: device_create failed\n");
        goto err_device;
    }

    pr_info("galvo: driver probed successfully (hrtimer + spi_async async architecture)\n");
    return 0;

err_device:
    class_destroy(galvo_class);
err_class:
    cdev_del(&galvo_cdev);
err_cdev:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

static int galvo_remove(struct spi_device *spi)
{
    hrtimer_cancel(&galvo_timer);

    if (batch_buffer) {
        vfree(batch_buffer);
        batch_buffer = NULL;
    }

    if (galvo_pwm_dev) pwm_disable(galvo_pwm_dev);

    device_destroy(galvo_class, dev_num);
    class_destroy(galvo_class);
    cdev_del(&galvo_cdev);
    unregister_chrdev_region(dev_num, 1);
    galvo_spi_device = NULL;

    pr_info("galvo: driver removed\n");
    return 0;
}

const struct of_device_id galvo_of_match[] = {
    { .compatible = "galvo_laser", },
    { }
};

struct spi_driver spi_galvo = {
    .driver = {
        .name = "galvo_laser",
        .owner = THIS_MODULE,
        .of_match_table = galvo_of_match,
    },
    .probe = galvo_probe,
    .remove = galvo_remove,
};

int galvo_driver_init(void) { return spi_register_driver(&spi_galvo); }
void galvo_driver_exit(void) { spi_unregister_driver(&spi_galvo); }

module_init(galvo_driver_init);
module_exit(galvo_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lei");
MODULE_DESCRIPTION("Industrial galvo laser marking driver - hrtimer + spi_async async architecture with vmalloc batch processing");
