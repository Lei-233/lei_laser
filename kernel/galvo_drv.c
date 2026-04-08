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

    return 0;
}


static int galvo_remove(struct spi_device *spi)
{
    pr_info("galvo SPI device removed\n");

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
