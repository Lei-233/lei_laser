#include <linux/init.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/vmalloc.h>

#include "galvo_ioctl.h"

// 在pwm_laser_drv.c里注册和注销平台驱动时会调用这两个函数
int galvo_driver_init(void);
void galvo_driver_exit(void);

dev_t dev_num;
struct cdev galvo_cdev;
struct class *galvo_class;
struct device *galvo_device;

static struct spi_device *galvo_spi_device;
static struct gpio_desc *ldac_gpiod;

/* --- hrtimer batch state --- */
static struct hrtimer galvo_hrtimer;
static struct galvo_ab *batch_coords = NULL;
static uint32_t batch_count = 0;
static uint32_t batch_idx = 0;
static ktime_t batch_interval;
static struct spi_message batch_msg;
static struct spi_transfer batch_xfer[2];
static uint8_t *batch_tx_buf = NULL;
/* --------------------------- */

static inline uint16_t clamp16(uint16_t v)
{
    return v; /* uint16_t已经是0-65535范围 */
}

// 给 galvo_ioctl.c 用：设置振镜 DAC 原始值（16bit）
int galvo_apply_ab(uint16_t a, uint16_t b);

// ioctl 处理函数在 pwm_laser_ioctl.c 里实现
extern long galvo_ioctl(struct file *, unsigned int, unsigned long);

/* DAC8562 命令定义 (24位SPI帧)
 * 格式: [CMD8][DATA_HIGH8][DATA_LOW8]
 */
#define DAC8562_CMD_WRITE_A_UPDATE   0x18  /* 写DAC-A并更新输出 */
#define DAC8562_CMD_WRITE_B_UPDATE   0x19  /* 写DAC-B并更新输出 */
#define DAC8562_CMD_WRITE_AB_UPDATE  0x1F  /* 同时写双通道并更新 */
#define DAC8562_CMD_POWERUP          0x20  /* 上电控制 */
#define DAC8562_CMD_RESET            0x28  /* 软件复位 */
#define DAC8562_CMD_INTERNAL_REF     0x38  /* 内部参考控制 */

#define DAC8562_DATA_POWERUP_AB      0x0003  /* 上电DAC-A和DAC-B */
#define DAC8562_DATA_RESET           0x0001  /* 复位到零 */
#define DAC8562_DATA_REF_ENABLE      0x0001  /* 使能内部参考，增益=2 */

/* DAC8562写入24位命令 */
static int dac8562_write(uint8_t cmd, uint16_t data)
{
    uint8_t tx[3];

    if (!galvo_spi_device)
        return -ENODEV;

    tx[0] = cmd;
    tx[1] = (data >> 8) & 0xFF;
    tx[2] = data & 0xFF;

    return spi_write(galvo_spi_device, tx, 3);
}

/* DAC8562初始化序列 */
static int dac8562_init(void)
{
    int ret;

    /* 1. 软件复位 */
    ret = dac8562_write(DAC8562_CMD_RESET, DAC8562_DATA_RESET);
    if (ret < 0) {
        pr_err("DAC8562 reset failed: %d\n", ret);
        return ret;
    }
    udelay(10);

    /* 2. 使能内部参考，增益=2（输出0-5V） */
    ret = dac8562_write(DAC8562_CMD_INTERNAL_REF, DAC8562_DATA_REF_ENABLE);
    if (ret < 0) {
        pr_err("DAC8562 enable internal ref failed: %d\n", ret);
        return ret;
    }
    udelay(10);

    /* 3. 上电双通道 */
    ret = dac8562_write(DAC8562_CMD_POWERUP, DAC8562_DATA_POWERUP_AB);
    if (ret < 0) {
        pr_err("DAC8562 power up failed: %d\n", ret);
        return ret;
    }
    udelay(10);

    /* 4. 初始输出到中间值 */
    ret = dac8562_write(DAC8562_CMD_WRITE_A_UPDATE, 32768);
    if (ret < 0) return ret;
    ret = dac8562_write(DAC8562_CMD_WRITE_B_UPDATE, 32768);
    if (ret < 0) return ret;

    pr_info("DAC8562 initialized successfully\n");
    return 0;
}


static int galvo_open(struct inode *inode, struct file *file)
{
    pr_info("galvo device opened\n");
    return 0;
}

static int galvo_release(struct inode *inode, struct file *file)
{
    pr_info("galvo device closed\n");
    return 0;
}

static ssize_t galvo_read(struct file *file, char __user *buf, size_t len, loff_t *offset)
{
    return 0;
}

/*
 * Legacy/debug interface:
 * write() expects struct galvo_ab and directly updates DAC.
 * Main control path should use ioctl.
 */
static ssize_t galvo_write(struct file *file, const char __user *buf, size_t len, loff_t *offset)
{
    struct galvo_ab ab;

    if (len < sizeof(ab))
        return -EINVAL;

    if (copy_from_user(&ab, buf, sizeof(ab)))
        return -EFAULT;

    galvo_apply_ab(ab.a, ab.b);
    return sizeof(ab);
}


struct file_operations galvo_fops = {
    .owner          = THIS_MODULE,
    .open           = galvo_open,
    .write          = galvo_write,
    .release        = galvo_release,
    .read           = galvo_read,
    .unlocked_ioctl = galvo_ioctl,
};

/* a、b的范围为0~65535，对应DAC8562的16位满量程输出 */
int galvo_apply_ab(uint16_t a, uint16_t b)
{
    int ret;

    if (!galvo_spi_device)
        return -ENODEV;

    /* DAC8562使用CMD_WRITE_x_UPDATE命令，写入即更新，无需LDAC */
    ret = dac8562_write(DAC8562_CMD_WRITE_A_UPDATE, clamp16(a));
    if (ret < 0) return ret;

    ret = dac8562_write(DAC8562_CMD_WRITE_B_UPDATE, clamp16(b));
    if (ret < 0) return ret;

    return 0;
}

static enum hrtimer_restart galvo_hrtimer_callback(struct hrtimer *timer)
{
    uint16_t a, b;

    if (batch_idx >= batch_count || !batch_coords || !galvo_spi_device || !batch_tx_buf) {
        return HRTIMER_NORESTART;
    }

    /* 从缓冲区获取坐标 */
    a = clamp16(batch_coords[batch_idx].a);
    b = clamp16(batch_coords[batch_idx].b);
    
    /* 组装 SPI 消息，使用提前分配好并配置好的 batch_msg 与 batch_xfer */
    batch_tx_buf[0] = DAC8562_CMD_WRITE_A_UPDATE;
    batch_tx_buf[1] = (a >> 8) & 0xFF;
    batch_tx_buf[2] = a & 0xFF;
    
    batch_tx_buf[3] = DAC8562_CMD_WRITE_B_UPDATE;
    batch_tx_buf[4] = (b >> 8) & 0xFF;
    batch_tx_buf[5] = b & 0xFF;

    /* 发送异步 SPI 消息 (硬中断/软中断安全的) */
    spi_async(galvo_spi_device, &batch_msg);

    batch_idx++;
    if (batch_idx >= batch_count) {
        return HRTIMER_NORESTART;
    }

    /* 设定下一个微秒级周期 */
    hrtimer_forward_now(timer, batch_interval);
    return HRTIMER_RESTART;
}

int galvo_apply_batch(struct galvo_ab_batch *batch)
{
    if (!galvo_spi_device || !batch_tx_buf)
        return -ENODEV;

    if (batch->count == 0 || batch->count > 1000000)
        return -EINVAL;

    /* 停止正在运行的定时器以免并发修改 */
    hrtimer_cancel(&galvo_hrtimer);

    if (batch_coords) {
        vfree(batch_coords);
        batch_coords = NULL;
    }

    /* 从用户空间拷贝大块数据内容，使用 vmalloc */
    batch_coords = vmalloc(batch->count * sizeof(struct galvo_ab));
    if (!batch_coords)
        return -ENOMEM;

    if (copy_from_user(batch_coords, batch->coords, batch->count * sizeof(struct galvo_ab))) {
        vfree(batch_coords);
        batch_coords = NULL;
        return -EFAULT;
    }

    batch_count = batch->count;
    batch_idx = 0;
    batch_interval = ktime_set(0, batch->interval_us * 1000ULL);

    /* 启动核心定时器 */
    hrtimer_start(&galvo_hrtimer, batch_interval, HRTIMER_MODE_REL);
    
    pr_info("galvo hrtimer started: %d points, interval %u us\n", batch->count, batch->interval_us);
    return 0;
}

int galvo_probe(struct spi_device *spi)
{
    int ret;

    printk("galvo SPI device probed (DAC8562)\n");

    galvo_spi_device = spi;
    spi_set_drvdata(spi, galvo_spi_device);

    /* DAC8562使用SPI Mode 1 (CPOL=0, CPHA=1) */
    spi->mode = SPI_MODE_1;
    spi->bits_per_word = 8;


    /* DAC8562最大支持50MHz，这里保守设置 */
    if (!spi->max_speed_hz)
        spi->max_speed_hz = 20000000;

    ret = spi_setup(spi);
    if (ret) {
        pr_err("spi_setup failed: %d\n", ret);
        return ret;
    }

    /* 获取 LDAC：对应 DTS 的 ldac-gpios（DAC8562可选，使用立即更新命令时不需要） */
    ldac_gpiod = devm_gpiod_get_optional(&spi->dev, "ldac", GPIOD_OUT_LOW);
    if (IS_ERR(ldac_gpiod)) {
        pr_err("failed to get ldac gpio: %ld\n", PTR_ERR(ldac_gpiod));
        return PTR_ERR(ldac_gpiod);
    }
    if (ldac_gpiod)
        pr_info("LDAC gpio ready (optional for DAC8562)\n");
    else
        pr_info("LDAC gpio not provided (using immediate update mode)\n");

    /* 初始化DAC8562 */
    ret = dac8562_init();
    if (ret) {
        pr_err("DAC8562 init failed: %d\n", ret);
        return ret;
    }

    ret = alloc_chrdev_region(&dev_num, 0, 1, "galvo");
    if (ret < 0) {
        pr_err("Failed to allocate char device region\n");
        return ret;
    }

    cdev_init(&galvo_cdev, &galvo_fops);
    ret = cdev_add(&galvo_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("Failed to add cdev\n");
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

    galvo_class = class_create(THIS_MODULE, "galvo_class");
    if (IS_ERR(galvo_class)) {
        pr_err("Failed to create class\n");
        cdev_del(&galvo_cdev);
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(galvo_class);
    }

    galvo_device = device_create(galvo_class, NULL, dev_num, NULL, "galvo");
    if (IS_ERR(galvo_device)) {
        pr_err("Failed to create device\n");
        class_destroy(galvo_class);
        cdev_del(&galvo_cdev);
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(galvo_device);
    }
    
    pr_info("DAC8562: spi mode=0x%x cs=%d speed=%dHz\n",
            spi->mode, spi->chip_select, spi->max_speed_hz);

    /* 分配 DMA 安全的发送缓冲区 */
    batch_tx_buf = devm_kzalloc(&spi->dev, 6, GFP_KERNEL | GFP_DMA);
    if (!batch_tx_buf) {
        pr_err("Failed to allocate DMA buffer for hrtimer\n");
    } else {
        /* 初始化 spi_message 以供 hrtimer 共用 */
        spi_message_init(&batch_msg);
        
        batch_xfer[0].tx_buf = batch_tx_buf;
        batch_xfer[0].len = 3;
        batch_xfer[0].cs_change = 1; /* 第二次发B数据前保持片选活动 */
        spi_message_add_tail(&batch_xfer[0], &batch_msg);

        batch_xfer[1].tx_buf = batch_tx_buf + 3;
        batch_xfer[1].len = 3;
        batch_xfer[1].cs_change = 0;
        spi_message_add_tail(&batch_xfer[1], &batch_msg);

        /* 初始化 hrtimer 定时器 */
        hrtimer_init(&galvo_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
        galvo_hrtimer.function = galvo_hrtimer_callback;
    }

    return 0;
}


static int galvo_remove(struct spi_device *spi)
{
    pr_info("galvo SPI device removed\n");

    hrtimer_cancel(&galvo_hrtimer);
    if (batch_coords) {
        vfree(batch_coords);
        batch_coords = NULL;
    }

    if (galvo_device)
        device_destroy(galvo_class, dev_num);
    if (galvo_class)
        class_destroy(galvo_class);

    cdev_del(&galvo_cdev);
    unregister_chrdev_region(dev_num, 1);

    galvo_device = NULL;
    galvo_class = NULL;
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


int galvo_driver_init(void)
{
    return spi_register_driver(&spi_galvo);
}

void galvo_driver_exit(void)
{
    spi_unregister_driver(&spi_galvo);
}

module_init(galvo_driver_init);
module_exit(galvo_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lei");
