# lei_laser - 基于RK3566的激光打标机

## 项目概述

学习Linux嵌入式驱动和系统编程的完整项目。

- **硬件平台**: RK3566 (泰山派开发板, Buildroot)
- **交叉编译工具链**: gcc-linaro-6.3.1 (aarch64)

## 系统架构

```
用户应用层 (user/)
    ├── galvo_app     振镜测试程序
    ├── galvo_hal     振镜HAL层
    ├── pwm_app       激光测试程序
    └── pwm_laser_hal 激光HAL层
        │
        ├── ioctl接口
        │
内核驱动层 (kernel/)
    ├── galvo.ko      振镜DAC驱动 (SPI → DAC8562SDGSR)
    └── pwm_laser.ko  激光PWM驱动
        │
硬件层
    ├── DAC8562SDGSR   16位双通道DAC (振镜X/Y轴)
    ├── PWM控制器      激光功率控制
    └── UART           串口通信(上位机)
```

## 模块详情

### 1. 激光器PWM控制模块 (pwm_laser.ko)

| 项目 | 说明 |
|------|------|
| 功能 | 通过PWM信号控制激光器功率 |
| 精度 | 1000级调节 (0-1000千分比) |
| 设备节点 | /dev/pwm_laser |
| ioctl | SET_TARGET, GET_TARGET |
| 状态 | 已完成 |

### 2. 振镜DAC控制模块 (galvo.ko)

| 项目 | 说明 |
|------|------|
| 功能 | 通过SPI控制DAC8562SDGSR输出模拟信号驱动振镜 |
| DAC芯片 | DAC8562SDGSR (TI, 16位双通道) |
| SPI模式 | Mode 1 (CPOL=0, CPHA=1) |
| 精度 | 65536级 (0-65535, 对应0-5V) |
| 设备节点 | /dev/galvo |
| ioctl | SET_AB, HOME, ENABLE, SET_BATCH (hrtimer异步批处理) |
| 高级特性 | 使用 hrtimer + spi_async 实现微秒级无抖动坐标输出 |
| 测试模式 | 正方形扫描、圆形扫描 |
| 状态 | 已完成 |

### 3. 串口通信模块 (计划中)

| 项目 | 说明 |
|------|------|
| 功能 | 与上位机通信，模拟Grbl协议 |
| 参考 | Arduino参考代码 (Grbl 1.1f兼容) |
| 命令支持 | G0/G1运动、G90/G91坐标模式、M3/M5激光控制 |
| 状态 | 待开发 |

## 设备树配置

- **主DTS**: tspi-rk3566-user-v10-linux.dts
- **SPI3节点**: 配置振镜DAC
- **PWM8节点**: 配置激光器
- **UART3节点**: 配置串口通信

## 编译说明

```bash
# 设置交叉编译环境
export ARCH=arm64
export CROSS_COMPILE=/.../aarch64-linux-gnu-

# 编译内核模块
cd kernel && make

# 编译用户程序
cd user && make

# 部署和测试
insmod galvo.ko        # 单独加载振镜模块
insmod pwm_laser.ko    # 单独加载激光模块
./galvo_app 0          # 正方形测试
./galvo_app 1          # 圆形测试
./pwm_app 500          # 50%功率测试
```

## 目录结构

```
lei_laser/
├── kernel/           # 内核驱动模块
│   ├── galvo_drv.c       # 振镜DAC驱动
│   ├── galvo_ioctl.h     # 振镜ioctl定义
│   ├── pwm_laser_drv.c   # 激光PWM驱动
│   ├── pwm_laser_ioctl.h # 激光ioctl定义
│   └── Makefile
├── user/             # 用户空间程序
│   ├── galvo_app.c       # 振镜测试程序
│   ├── galvo_hal.c       # 振镜HAL层
│   ├── galvo_hal.h
│   ├── pwm_app.c         # 激光测试程序
│   ├── pwm_laser_hal.c   # 激光HAL层
│   ├── pwm_laser_hal.h
│   └── Makefile
├── docs/             # 项目文档
│   ├── PWM_Laser_Guide.md       # 激光功率测试指南
│   ├── UART_Controller_Guide.md # 串口控制器使用指南
│   └── SPI_Signal_Guide.md      # SPI信号测试指南
├── reference/        # 参考资料
│   └── 参考代码Arduino_Ji_Guang_Da_Biao_Ji.txt
├── PROJECT_SUMMARY.md
└── PROGRESS.md
```
