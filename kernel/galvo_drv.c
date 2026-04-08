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

#include "galvo_ioctl.h"

int galvo_driver_init(void);
void galvo_driver_exit(void);

dev_t dev_num;
struct cdev galvo_cdev;
struct class *galvo_class;
struct device *galvo_device;

static struct spi_device *galvo_spi_device;

static struct pwm_device *galvo_pwm_dev;

static struct hrtimer galvo_timer;

static struct galvo_point *batch_buffer = NULL;
static uint32_t batch_count = 0;
static uint32_t current_point_idx = 0;

static struct spi_message spi_msg_a;
static struct spi_message spi_msg_b;
static struct spi_transfer spi_xfer_a;
static struct spi_transfer spi_xfer_b;
static uint8_t spi_tx_buf[6];

// ioctl 处理函数在 galvo_ioctl.c 里实现
extern long galvo_ioctl(struct file *, unsigned int, unsigned long);

/* DAC8562 命令定义 */
#define DAC8562_CMD_WRITE_A_UPDATE   0x18
#define DAC8562_CMD_WRITE_B_UPDATE   0x19
#define DAC8562_CMD_WRITE_AB_UPDATE  0x1F
#define DAC8562_CMD_POWERUP          0x20
#define DAC8562_CMD_RESET            0x28
#define DAC8562_CMD_INTERNAL_REF     0x38

#define DAC8562_DATA_POWERUP_AB      0x0003
#define DAC8562_DATA_RESET           0x0001
#define DAC8562_DATA_REF_ENABLE      0x0001

static inline uint16_t clamp16(uint16_t v) { return v; }

/* 遗留的普通发送方式 */
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
    ret = dac8562_write(DAC8562_CMD_RESET, DAC8562_DATA_RESET);
    if (ret < 0) return ret;
    udelay(10);
    ret = dac8562_write(DAC8562_CMD_INTERNAL_REF, DAC8562_DATA_REF_ENABLE);
    if (ret < 0) return ret;
    udelay(10);
    ret = dac8562_write(DAC8562_CMD_POWERUP, DAC8562_DATA_POWERUP_AB);
    if (ret < 0) return ret;
    udelay(10);
    ret = dac8562_write(DAC8562_CMD_WRITE_A_UPDATE, 32768);
    if (ret < 0) return ret;
    ret = dac8562_write(DAC8562_CMD_WRITE_B_UPDATE, 32768);
    return ret;
}

/* 高精度定时器回调，完全脱离内核休眠调度 */
static enum hrtimer_restart galvo_timer_callback(struct hrtimer *timer)
{
    struct galvo_point *pt;
    uint32_t duty_ns;

    if (current_point_idx >= batch_count || !batch_buffer) {
        if (galvo_pwm_dev) pwm_config(galvo_pwm_dev, 0, 1000000);
        return HRTIMER_NORESTART;
    }

    pt = &batch_buffer[current_point_idx];

    /* 1. 同步进行 PWM 更新 */
    duty_ns = (pt->pwm_duty > 1000 ? 1000 : pt->pwm_duty) * 1000;
    if (galvo_pwm_dev) {
        pwm_config(galvo_pwm_dev, duty_ns, 1000000);
    }

    /* 2. 纯异步非阻塞提交SPI任务 */
    spi_tx_buf[0] = DAC8562_CMD_WRITE_A_UPDATE;
    spi_tx_buf[1] = (pt->x >> 8) & 0xFF;
    spi_tx_buf[2] = pt->x & 0xFF;

    spi_tx_buf[3] = DAC8562_CMD_WRITE_B_UPDATE;
    spi_tx_buf[4] = (pt->y >> 8) & 0xFF;
    spi_tx_buf[5] = pt->y & 0xFF;

    if (galvo_spi_device) {
        spi_async(galvo_spi_device, &spi_msg_a);
        spi_async(galvo_spi_device, &spi_msg_b);
    }

    current_point_idx++;

    if (current_point_idx >= batch_count) {
        if (galvo_pwm_dev) pwm_config(galvo_pwm_dev, 0, 1000000); // 运行完毕后关光
        return HRTIMER_NORESTART;
    }

    /* 将定时器推向下一个延时节点，实现微秒级精确步进 */
    hrtimer_forward_now(timer, ns_to_ktime(pt->delay_us * 1000ULL));
    return HRTIMER_RESTART;
}

/* 提供给 ioctl 使用的全新批下发通道 */
int galvo_start_batch(struct galvo_point __user *user_points, uint32_t count)
{
    if (count == 0) return 0;
    
    hrtimer_cancel(&galvo_timer);
    
    if (batch_buffer) {
        vfree(batch_buffer);
        batch_buffer = NULL;
    }

    // 突破 kmalloc 的大块内存碎片化束缚
    batch_buffer = vmalloc(count * sizeof(struct galvo_point));
    if (!batch_buffer) {
        pr_err("galvo: failed to vmalloc %u points\n", count);
        return -ENOMEM;
    }

    // 不可越过安检线死穴：必须利用 copy_from_user 从安全网捞数据
    if (copy_from_user(batch_buffer, user_points, count * sizeof(struct galvo_point))) {
        vfree(batch_buffer);
        batch_buffer = NULL;
        return -EFAULT;
    }

    batch_count = count;
    current_point_idx = 0;

    if (galvo_pwm_dev) {
        pwm_enable(galvo_pwm_dev);
    }

    // 第一帧直接由定时器驱动起飞
    hrtimer_start(&galvo_timer, ktime_set(0, 0), HRTIMER_MODE_REL);

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
    int ret;

    galvo_spi_device = spi;
    spi_set_drvdata(spi, galvo_spi_device);

    spi->mode = SPI_MODE_1;
    spi->bits_per_word = 8;
    if (!spi->max_speed_hz) spi->max_speed_hz = 20000000;
    
    ret = spi_setup(spi);
    if (ret) return ret;

    /* 初始化用于异步的 SPI 消息结构 */
    spi_message_init(&spi_msg_a);
    spi_xfer_a.tx_buf = &spi_tx_buf[0];
    spi_xfer_a.len = 3;
    spi_message_add_tail(&spi_xfer_a, &spi_msg_a);

    spi_message_init(&spi_msg_b);
    spi_xfer_b.tx_buf = &spi_tx_buf[3];
    spi_xfer_b.len = 3;
    spi_message_add_tail(&spi_xfer_b, &spi_msg_b);

    /* 获取 PWM 节点，合并控制 */
    galvo_pwm_dev = devm_pwm_get(&spi->dev, NULL);
    if (IS_ERR(galvo_pwm_dev)) {
        galvo_pwm_dev = NULL; 
    } else {
        pwm_config(galvo_pwm_dev, 0, 1000000);
        pwm_set_polarity(galvo_pwm_dev, PWM_POLARITY_NORMAL);
    }
    
    /* 建立超高精度内核驱动定时器 */
    hrtimer_init(&galvo_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    galvo_timer.function = galvo_timer_callback;

    dac8562_init();

    ret = alloc_chrdev_region(&dev_num, 0, 1, "galvo");
    cdev_init(&galvo_cdev, &galvo_fops);
    ret = cdev_add(&galvo_cdev, dev_num, 1);

    galvo_class = class_create(THIS_MODULE, "galvo_class");
    galvo_device = device_create(galvo_class, NULL, dev_num, NULL, "galvo");

    return 0;
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
