/* 激光打标机串口控制器 - 全异步批处理重构版 (Grbl协议兼容) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>

#include "galvo_hal.h"

/* ================= 配置参数 ================= */
#define UART_DEVICE "/dev/ttyS3"
#define UART_BAUDRATE B115200

#define BEILV 100.0
#define DAC_MAX 65535
#define STEP_NUM 0.1

#define DEFAULT_FEED_RATE 10000.0
#define G0_FEED_RATE 20000.0
#define LASER_S_MAX 1000.0

#define ROTATE_180 1

#define CMD_BUF_SIZE 256
#define STATUS_INTERVAL_MS 200

/* ================= 异步缓冲池架构 ================= */
/* 上限设立为一次5万点位缓存容纳量 */
#define MAX_POOL_SIZE 50000
static struct galvo_point g_point_pool[MAX_POOL_SIZE];
static uint32_t g_pool_count = 0;
static uint64_t g_accumulated_time_us = 0;

/* ================= 全局状态 ================= */
static volatile int running = 1;

static double currentX = 0.0;
static double currentY = 0.0;
static double currentFeedRate = DEFAULT_FEED_RATE;

static int laserEnabled = 0;
static double currentSPower = 0.0;

static int isAbsoluteMode = 1;
static int uart_fd = -1;
static struct timespec lastStatusTime;

/* ================= 声明 ================= */
static void trigger_batch_flush(void);
static void send_status_report(void);

static void signal_handler(int sig) {
    if (sig == SIGINT) {
        printf("\n🛑 紧急停止！正在安全关闭系统...\n");
        running = 0;
    }
}

static int timespec_diff_ms(struct timespec *start, struct timespec *end) {
    long sec_diff = end->tv_sec - start->tv_sec;
    long nsec_diff = end->tv_nsec - start->tv_nsec;
    return (int)(sec_diff * 1000 + nsec_diff / 1000000);
}

static int uart_init(const char *device, speed_t baudrate) {
    int fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) return -1;
    struct termios options;
    tcgetattr(fd, &options);
    cfsetispeed(&options, baudrate);
    cfsetospeed(&options, baudrate);
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    options.c_cflag |= CLOCAL | CREAD;
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    options.c_oflag &= ~OPOST;
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 1;
    tcflush(fd, TCIFLUSH);
    if (tcsetattr(fd, TCSANOW, &options) != 0) {
        close(fd); return -1;
    }
    return fd;
}

static void uart_send(const char *msg) {
    if (uart_fd < 0 || !msg) return;
    write(uart_fd, msg, strlen(msg));
}

static int uart_read_line(char *buf, int size, int timeout_ms) {
    if (!buf || size <= 0) return -1;
    int idx = 0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (running && idx < size - 1) {
        char c;
        int n = read(uart_fd, &c, 1);
        if (n > 0) {
            if (c == '?') {
                send_status_report();
                continue; 
            }
            if (c == '\n' || c == '\r') {
                if (idx > 0) { buf[idx] = '\0'; return idx; }
            } else {
                buf[idx++] = c;
            }
        } else {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (timespec_diff_ms(&start, &now) > timeout_ms) break;
            usleep(1000);
        }
    }
    buf[idx] = '\0';
    return idx;
}

static uint16_t mm_to_dac(double mm) {
    double val = mm * BEILV;
#if ROTATE_180
    val = DAC_MAX - val;
#endif
    if (val < 0) val = 0;
    if (val > DAC_MAX) val = DAC_MAX;
    return (uint16_t)val;
}

/* 核心再造机制：积攒打点数据代替死等 */
static void perform_move(double targetX, double targetY, double feedRateMmMin, int is_fast_move) {
    double dx = targetX - currentX;
    double dy = targetY - currentY;
    double distance = sqrt(dx * dx + dy * dy);

    if (distance < STEP_NUM) {
        currentX = targetX;
        currentY = targetY;
        // 如果过小也当作一步加入
        if (g_pool_count < MAX_POOL_SIZE) {
            g_point_pool[g_pool_count].x = mm_to_dac(currentX);
            g_point_pool[g_pool_count].y = mm_to_dac(currentY);
            g_point_pool[g_pool_count].pwm_duty = (laserEnabled && !is_fast_move) ? currentSPower : 0;
            g_point_pool[g_pool_count].delay_us = 100; //基础延时
            g_accumulated_time_us += 100;
            g_pool_count++;
        }
        return;
    }

    int steps = (int)(distance / STEP_NUM);
    if (steps < 1) steps = 1;

    double stepDx = dx / steps;
    double stepDy = dy / steps;

    double feedRateSec = feedRateMmMin / 60.0;
    if (feedRateSec < 0.1) feedRateSec = 0.1;

    double totalTimeSec = distance / feedRateSec;
    double stepTimeUs = (totalTimeSec * 1000000.0) / steps;
    if (stepTimeUs < 100) stepTimeUs = 100;

    for (int i = 1; i <= steps && running; i++) {
        currentX += stepDx;
        currentY += stepDy;

        if (g_pool_count >= MAX_POOL_SIZE) {
            // 防溢出自我保护
            trigger_batch_flush();
        }

        g_point_pool[g_pool_count].x = mm_to_dac(currentX);
        g_point_pool[g_pool_count].y = mm_to_dac(currentY);
        // 全异步架构下占空比直接绑进数组，高速空移时强闭光
        g_point_pool[g_pool_count].pwm_duty = (laserEnabled && !is_fast_move) ? currentSPower : 0;
        g_point_pool[g_pool_count].delay_us = (uint32_t)stepTimeUs;

        g_accumulated_time_us += (uint32_t)stepTimeUs;
        g_pool_count++;
    }

    currentX = targetX;
    currentY = targetY;
}

/* 核心触发：下潜式透传阵列及模拟拦截响应 */
static void trigger_batch_flush(void) {
    if (g_pool_count > 0) {
        /*
         * 调用 ioctl 新设立的高级接口，此时包裹会被 vmalloc 无阻抗吞并。
         * ioctl 函数会执行 1 毫秒马上返回！
         */
        if (galvo_send_batch(g_point_pool, g_pool_count) < 0) {
            perror("galvo_send_batch err");
        }
        
        /* 
         * 由于 ioctl 秒返回脱离了堵塞，
         * 我们作为通讯中间件，需要代替上位机去匹配内核 hrtimer 真实的延展消费时间！
         * 否则主机爆传数据过来会取消/冲刷掉内核中还没来得及由中断走完的数据池。
         */
        if (g_accumulated_time_us > 0) {
            usleep(g_accumulated_time_us);
        }

        g_pool_count = 0;
        g_accumulated_time_us = 0;
    }
}

static void send_status_report(void) {
    char buf[128];
    snprintf(buf, sizeof(buf), "<Idle|MPos:%.3f,%.3f,0.000|WPos:%.3f,%.3f,0.000|FS:%.0f,%.0f>\r\n",
             currentX, currentY, currentX, currentY, currentFeedRate, currentSPower);
    uart_send(buf);
}

static void process_gcode(const char *line) {
    if (!line || strlen(line) == 0) return;

    char cmd[CMD_BUF_SIZE];
    strncpy(cmd, line, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    for (int i = 0; cmd[i]; i++) cmd[i] = toupper(cmd[i]);

    if (strcmp(cmd, "?") == 0) return; 

    char *p = cmd;
    if (cmd[0] == '$') {
        if (strncmp(cmd, "$J=", 3) == 0) p = cmd + 3;
        else {
            if (strcmp(cmd, "$I") == 0) {
                uart_send("[VER:1.1f.Linux:BlackDandelion]\r\n[OPT:V,15,128]\r\n");
            } else if (strcmp(cmd, "$G") == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "[GC:G0 G54 G17 G21 G%d G94 M%d T0 F%.0f S%.0f]\r\n",
                         isAbsoluteMode ? 90 : 91, laserEnabled ? 3 : 5, currentFeedRate, currentSPower);
                uart_send(buf);
            }
            uart_send("ok\r\n");
            return;
        }
    }

    double x_val = 0.0, y_val = 0.0, feedRate = currentFeedRate, sPower = currentSPower;
    int hasX = 0, hasY = 0, hasF = 0, hasS = 0;
    int motion_mode = -1, laser_mode = -1;

    while (*p) {
        char letter = *p++;
        if (!isalpha(letter)) continue;
        char *endptr;
        double value = strtod(p, &endptr);
        if (endptr == p) continue;
        p = endptr;
        switch (letter) {
            case 'G': 
                if (value == 0 || value == 1 || value == 92) motion_mode = (int)value;
                else if (value == 90) isAbsoluteMode = 1;
                else if (value == 91) isAbsoluteMode = 0;
                break;
            case 'M': 
                if (value == 3 || value == 4) laser_mode = 1;
                else if (value == 5) laser_mode = 0;
                else if (value == 2 || value == 30) laser_mode = 2; // 端点闭合
                break;
            case 'X': x_val = value; hasX = 1; break;
            case 'Y': y_val = value; hasY = 1; break;
            case 'F': feedRate = value; hasF = 1; break;
            case 'S': sPower = value; hasS = 1; break;
        }
    }

    if (hasF) currentFeedRate = feedRate;
    if (hasS) {
        currentSPower = sPower;
        if (currentSPower > 1000) currentSPower = 1000;
        if (currentSPower < 0) currentSPower = 0;
    } 

    if (laser_mode == 1) laserEnabled = 1;
    else if (laser_mode == 0 || laser_mode == 2) laserEnabled = 0;

    double targetX = currentX, targetY = currentY;
    if (hasX) targetX = isAbsoluteMode ? x_val : (currentX + x_val);
    if (hasY) targetY = isAbsoluteMode ? y_val : (currentY + y_val);

    if (motion_mode == 0) {
        if (hasX || hasY) perform_move(targetX, targetY, G0_FEED_RATE, 1);
    } else if (motion_mode == 1) {
        if (hasX || hasY || hasS) {
            double speed = currentFeedRate > 0 ? currentFeedRate : DEFAULT_FEED_RATE;
            if (hasX || hasY) perform_move(targetX, targetY, speed, 0);
        }
    } else if (motion_mode == 92) {
        currentX = 0.0; currentY = 0.0;
        trigger_batch_flush(); // 坐标系发生变动必须清理历史记录
    }

    /* 
     * “基于方案A”：段落触点刷新！
     * 在扫描完成了一段复杂的描边闭环时遇到了 M5 关闭激光命令。
     * 表示这个激光笔画画完了，借此机会一次将之前的数十万坐标倾泻进内核中。
     */
    if (laser_mode == 0 || laser_mode == 2) {
        trigger_batch_flush();
    }

    uart_send("ok\r\n");
}

int main(int argc, char *argv[]) {
    char target_device[256];
    strncpy(target_device, UART_DEVICE, sizeof(target_device));
    if (argc > 1) {
        strncpy(target_device, argv[1], sizeof(target_device) - 1);
        target_device[sizeof(target_device) - 1] = '\0';
    }

    printf("\n=== 激光打标机底层跨进程伺服控制器 ===\n");
    printf("核心: 用户态全异步批量缓冲架构 (Grbl 1.1f 接口支持)\n");
    printf("板卡控制句柄: /dev/galvo，映射通讯节点: %s \n\n", target_device);

    signal(SIGINT, signal_handler);

    if (galvo_open() < 0) {
        fprintf(stderr, "错误: 无法打开振镜设备 /dev/galvo\n");
        return 1;
    }
    printf("✅ 振镜全集成式系统层初始化成功\n");

    uart_fd = uart_init(target_device, UART_BAUDRATE);
    if (uart_fd < 0) {
        fprintf(stderr, "【致命错误】: 无法打开通讯节点 %s\n", target_device);
        galvo_close();
        return 1;
    }
    printf("✅ 硬件隔离串口侦听就绪\n\n");

    sleep(1); 
    uart_send("\r\n");
    uart_send("Grbl 1.1f ['$' for help]\r\n");

    printf("🚀 异步收集池已上膛，等待 LightBurn 或上位机冲洗注入矩阵指令...\n\n");

    clock_gettime(CLOCK_MONOTONIC, &lastStatusTime);
    char cmdBuf[CMD_BUF_SIZE];
    
    while (running) {
        int len = uart_read_line(cmdBuf, sizeof(cmdBuf), 100);
        if (len > 0) {
            char *start = cmdBuf;
            while (*start && isspace(*start)) start++;
            char *end = start + strlen(start) - 1;
            while (end > start && isspace(*end)) *end-- = '\0';
            if (strlen(start) > 0) process_gcode(start);
        }
    }

    printf("\n🔴 接收到终止指令，开始排空剩余数组...\n");
    trigger_batch_flush();

    close(uart_fd);
    galvo_close();
    printf("✅ 工作收尾清理完成安全关闭，再见。\n\n");
    return 0;
}
