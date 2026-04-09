# lei_laser - 工业级高性能激光打标系统驱动（基于 RK3566）

## 项目概述

基于 RK3566 平台（泰山派开发板, Buildroot Linux）的激光打标系统底层驱动研发。为解决通用外设接口系统调用开销大、时序无法严格对齐的问题，主动摆脱了对通用工具包（spidev）的依赖，通过重构代码实现了对振镜运动与激光发光动作的**微秒级精确同步控制**。

### 核心技术亮点

| 技术 | 描述 |
|:---|:---|
| **hrtimer 高精度定时器** | 直接绑定底层硬件计数器（非 jiffies 节拍），支持纳秒级 ktime_t，在非实时 Linux 中强行辟出微秒级硬实时快车道 |
| **spi_async 异步通信** | 中断回调中使用非阻塞异步接口，数据"嗖"地丢进硬件队列瞬间返回，杜绝休眠死机风险 |
| **SPI 双缓冲乒乓切换** | 解决 spi_async 返回后 DMA 仍在访问缓冲区的竞态，确保数据完整性 |
| **vmalloc 大缓冲池** | 突破 kmalloc 物理连续内存碎片化束缚，安全接纳万级坐标批量下发 |
| **自定义 ioctl 接口** | 5 个专用命令字，支持结构体透传、状态查询，替代低效的 write() 字节流 |
| **copy_from_user 安全搬运** | 严守内核/用户空间隔离铁律，绝不直接解引用用户态指针 |
| **spinlock 并发保护** | 保障中断上下文与进程上下文间共享状态的一致性 |

---

## 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                      用户应用层 (user/)                       │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────────┐  │
│  │  galvo_app   │  │uart_controller│  │   Qt GUI (计划)   │  │
│  │  振镜测试程序 │  │ Grbl协议控制器 │  │  触控界面 (MIPI)  │  │
│  └──────┬───────┘  └──────┬───────┘  └────────┬──────────┘  │
│         │                 │                    │             │
│  ┌──────┴─────────────────┴────────────────────┴──────────┐  │
│  │              galvo_hal (硬件抽象层)                      │  │
│  │  open / send_batch / set_ab / home / enable / status   │  │
│  └──────────────────────┬────────────────────────────────┘  │
│                         │ 唯一一次 ioctl 系统调用            │
├─────────────────────────┼───────────────────────────────────┤
│                   内核驱动层 (kernel/)                        │
│  ┌──────────────────────┴────────────────────────────────┐  │
│  │                  galvo.ko 字符设备                      │  │
│  │                                                        │  │
│  │  ioctl 分发 ──┬── SET_BATCH → vmalloc + copy_from_user │  │
│  │               ├── SET_AB    → dac8562_write (同步)     │  │
│  │               ├── HOME     → 回中点 2.5V + 关激光       │  │
│  │               ├── ENABLE   → 使能/禁用控制              │  │
│  │               └── STATUS   → 运行状态快照               │  │
│  │                                                        │  │
│  │  hrtimer ──→ spi_async (双缓冲) ──→ DAC8562 振镜       │  │
│  │         └──→ pwm_config           ──→ 激光 PWM         │  │
│  └────────────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│                        硬件层                                │
│  ┌────────────────┐  ┌──────────────┐  ┌────────────────┐  │
│  │ DAC8562SDGSR   │  │  PWM 控制器   │  │   UART3        │  │
│  │ 16位双通道DAC  │  │  激光功率调制  │  │  串口通信       │  │
│  │ SPI3 Mode1    │  │  1KHz/千分比  │  │  115200 8N1    │  │
│  │ X/Y 振镜偏转   │  │  0-100%精度  │  │  Grbl 1.1f     │  │
│  └────────────────┘  └──────────────┘  └────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

---

## 驱动模块详情 (galvo.ko)

### 集成功能

galvo.ko 是一个高度集成的字符设备驱动模块，在单个 .ko 中同时管理：
- **SPI 振镜控制**：通过 DAC8562SDGSR 驱动 X/Y 双轴振镜
- **PWM 激光控制**：在 hrtimer 中断帧内与振镜偏转同步触发
- **异步批处理引擎**：hrtimer + spi_async 流水线

### ioctl 接口清单

| 命令 | 方向 | 数据结构 | 功能描述 |
|:---|:---|:---|:---|
| `GALVO_IOC_SET_AB` | 用户→内核 | `galvo_ab` | 直接设置双通道 DAC 电压（单点控制） |
| `GALVO_IOC_HOME` | 无数据 | — | 振镜回中点 (2.5V)，停止定时器，关激光 |
| `GALVO_IOC_ENABLE` | 用户→内核 | `int` | 使能/禁用驱动（禁用时拒绝新任务） |
| `GALVO_IOC_SET_BATCH` | 用户→内核 | `galvo_batch` | 批量坐标下沉进内核 vmalloc 池 |
| `GALVO_IOC_QUERY_STATUS` | 内核→用户 | `galvo_status` | 查询运行状态、进度、SPI 错误数 |

### 关键技术参数

| 参数 | 值 |
|:---|:---|
| DAC 芯片 | DAC8562SDGSR (TI) |
| DAC 分辨率 | 16 位 (65536 级) |
| 输出范围 | 0-5V (内部参考 2.5V, 增益=2) |
| SPI 模式 | Mode 1 (CPOL=0, CPHA=1) |
| SPI 帧格式 | 24 位 [CMD 8bit][DATA 16bit] |
| SPI 最高时钟 | 50 MHz（驱动默认 20 MHz） |
| PWM 频率 | 1 KHz (1ms 周期) |
| 激光分辨率 | 1000 档 (0.1% 步进) |
| 定时器精度 | 纳秒级 (hrtimer + CLOCK_MONOTONIC) |
| 设备节点 | `/dev/galvo` |
| 设备树 compatible | `"galvo_laser"` |

---

## 用户空间程序

### galvo_app — 振镜批处理演示程序

在用户态堆内存中一口气计算完轨迹坐标（正方形/圆形），通过唯一一次 `ioctl(GALVO_IOC_SET_BATCH)` 透传给内核，然后应用层彻底解绑脱身。

### uart_controller — Grbl 协议串口控制器

兼容 Grbl 1.1f 协议的全异步串口控制器。接收上位机（LaserGRBL / LightBurn）发来的 G-code 命令，在用户态积攒坐标到缓冲池，在段落结束时一次性批量刷入内核。

| 支持命令 | 说明 |
|:---|:---|
| G0 X# Y# | 快速空移（自动关光） |
| G1 X# Y# F# | 直线插补（跟随激光状态） |
| G90 / G91 | 绝对/相对坐标模式 |
| M3 S# / M5 | 开/关激光，设置功率 |
| $I / $G / ? | 版本/状态/实时位置查询 |

---

## 目录结构

```
lei_laser/
├── kernel/               # 内核驱动模块
│   ├── galvo_drv.c           # 核心驱动（hrtimer + spi_async + PWM 集成）
│   ├── galvo_ioctl.c         # ioctl 命令分发处理（5 个命令）
│   ├── galvo_ioctl.h         # ioctl 接口定义（内核版本）
│   └── Makefile              # 交叉编译构建文件
├── user/                 # 用户空间程序
│   ├── galvo_app.c           # 振镜批处理演示程序
│   ├── galvo_hal.c           # 振镜硬件抽象层（封装 ioctl）
│   ├── galvo_hal.h           # HAL 接口声明
│   ├── galvo_ioctl.h         # ioctl 接口定义（用户版本）
│   ├── uart_controller.c    # Grbl 串口控制器（全异步架构）
│   └── CMakeLists.txt        # CMake 交叉编译配置
├── docs/                 # 项目文档
│   ├── PWM_Laser_Guide.md        # 激光功率测试指南
│   ├── SPI_Signal_Guide.md       # SPI 信号测试指南
│   └── UART_Controller_Guide.md  # 串口控制器使用指南
├── reference/            # 参考资料
│   └── 参考代码Arduino_Ji_Guang_Da_Biao_Ji.txt
├── PROJECT_SUMMARY.md    # 本文件
└── PROGRESS.md           # 开发进度日志
```

---

## 编译与部署

```bash
# ===== 编译内核模块 =====
cd kernel && make clean && make
# 产物: galvo.ko

# ===== 编译用户程序 =====
cd user && mkdir -p build && cd build && cmake .. && make
# 产物: galvo_app, uart_controller

# ===== 部署到开发板 =====
scp kernel/galvo.ko root@<IP>:/tmp/
scp user/build/galvo_app user/build/uart_controller root@<IP>:/tmp/

# ===== 板端运行 =====
insmod galvo.ko                    # 加载驱动
./galvo_app 0                      # 正方形批处理测试
./galvo_app 1                      # 圆形批处理测试
./uart_controller                  # 启动 Grbl 串口控制器
./uart_controller /dev/ttyS3       # 指定串口设备
```

---

## 硬件平台

- **主控芯片**: RK3566 (四核 A55, 原生 SPI/PWM/UART)
- **开发板**: 泰山派
- **系统**: Buildroot Linux
- **交叉编译**: gcc-linaro-6.3.1 (aarch64-linux-gnu)
- **内核源码**: /home/lei/tspi_linux_sdk/kernel
