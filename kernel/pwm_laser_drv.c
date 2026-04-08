#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pwm.h>
#include <linux/uaccess.h>

#include "pwm_laser_ioctl.h"

dev_t dev;

struct cdev pwm_laser_cdev;

struct class *pwm_laser_class;
struct device *pwm_laser_device;
struct pwm_device *pwm_laser_pwm_dev;

// 给 ioctl.c 用：提供一个"设置占空比千分比"的内核函数
int pwm_laser_apply_target(uint16_t percentage);

// ioctl 处理函数在 pwm_laser_ioctl.c 里实现
long pwm_laser_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

static int pwm_laser_open(struct inode *inode, struct file *file)
{
    pwm_config(pwm_laser_pwm_dev, 0, 1000000); // 1ms = 1000Hz
    pwm_set_polarity(pwm_laser_pwm_dev, PWM_POLARITY_NORMAL); //设置极性
    pwm_enable(pwm_laser_pwm_dev); //使能 pwm

    printk("pwm_laser_open\n");
    return 0;
}
static ssize_t pwm_laser_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    printk("pwm_laser_read\n");
    return 0;
}
static ssize_t pwm_laser_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    uint8_t percentage;

    if (count != sizeof(percentage))
        return -EINVAL;

    if (copy_from_user(&percentage, buf, sizeof(percentage)))
        return -EFAULT;

    pwm_laser_apply_target(percentage);
    return sizeof(percentage);

}
static int pwm_laser_release(struct inode *inode, struct file *file)
{
    printk("pwm_laser_release\n");
    return 0;
}


struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = pwm_laser_open,
    .read           = pwm_laser_read,
    .write          = pwm_laser_write,
    .release        = pwm_laser_release,
    .unlocked_ioctl = pwm_laser_ioctl,
};

int pwm_laser_apply_target(uint16_t percentage)
{
    uint32_t duty_ns;

    if (percentage > 1000) percentage = 1000;

    duty_ns = (percentage * 1000);
    pwm_config(pwm_laser_pwm_dev, duty_ns, 1000000); // 周期 1ms
    return 0;
}


int pwm_laser_probe(struct platform_device *pdev)
{
    int ret;

    ret = alloc_chrdev_region(&dev, 0, 1, "pwm_laser");
    if (ret < 0) return ret;

    cdev_init(&pwm_laser_cdev, &fops);
    ret = cdev_add(&pwm_laser_cdev, dev, 1);
    if (ret < 0) goto err_cdev;

    pwm_laser_class = class_create(THIS_MODULE, "pwm_laser_class");
    if (IS_ERR(pwm_laser_class)) { ret = PTR_ERR(pwm_laser_class); goto err_class; }

    pwm_laser_device = device_create(pwm_laser_class, NULL, dev, NULL, "pwm_laser");
    if (IS_ERR(pwm_laser_device)) { ret = PTR_ERR(pwm_laser_device); goto err_dev; }

    pwm_laser_pwm_dev = devm_of_pwm_get(&pdev->dev, pdev->dev.of_node, NULL);
    if (IS_ERR(pwm_laser_pwm_dev)) { ret = PTR_ERR(pwm_laser_pwm_dev); goto err_pwm; }

    printk("pwm_laser_probe\n");
    return 0;

err_pwm:
    device_destroy(pwm_laser_class, dev);
err_dev:
    class_destroy(pwm_laser_class);
err_class:
    cdev_del(&pwm_laser_cdev);
err_cdev:
    unregister_chrdev_region(dev, 1);
    return ret;
}

int pwm_laser_remove(struct platform_device *pdev)
{
    pwm_disable(pwm_laser_pwm_dev);

    device_destroy(pwm_laser_class, dev);
    class_destroy(pwm_laser_class);
    cdev_del(&pwm_laser_cdev);
    unregister_chrdev_region(dev, 1);

    printk("pwm_laser_remove\n");
    return 0;
}

const struct of_device_id pwm_laser_of_match[] = {
    {.compatible = "pwm_laser"},
    {},
};

struct platform_driver pwm_laser_platform_driver = {
    .driver = {
        .name = "pwm_laser",
        .of_match_table = pwm_laser_of_match,
        .owner = THIS_MODULE,
    },
    .probe = pwm_laser_probe,
    .remove = pwm_laser_remove,
};


int pwm_laser_init(void) 
{
    return platform_driver_register(&pwm_laser_platform_driver); //注册平台驱动
}

void pwm_laser_exit(void) 
{
    platform_driver_unregister(&pwm_laser_platform_driver);
}

module_init(pwm_laser_init);
module_exit(pwm_laser_exit); 
MODULE_LICENSE("GPL"); 
MODULE_AUTHOR("lei"); 
