/* 激光打标机串口控制器 - Grbl协议兼容版本 */
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

#include "galvo_hal.h"
#include "pwm_laser_hal.h"

/* ================= 配置参数 ================= */
#define UART_DEVICE "/dev/ttyS3"        //串口设备
#define UART_BAUDRATE B115200           //波特率

#define BEILV 100.0                     //坐标倍率：1mm对应DAC值(100 = 1mm对应100个DAC单位)
#define DAC_MAX 65535                   //DAC最大值(16位)
#define STEP_NUM 0.1                    //插补最小距离(mm)

#define DEFAULT_FEED_RATE 10000.0       //默认速度(mm/min)
#define G0_FEED_RATE 20000.0            //G0快速移动速度(mm/min)
#define LASER_S_MAX 1000.0              //激光功率最大值

#define ROTATE_180 1                    //1=图案旋转180度, 0=正常方向

#define CMD_BUF_SIZE 256                //命令缓冲区大小
#define STATUS_INTERVAL_MS 200          //状态报告间隔(ms)

/* ================= 全局状态 ================= */
static volatile int running = 1;       //运行标志

//机器状态
static double currentX = 0.0;          //当前X坐标(mm)
static double currentY = 0.0;          //当前Y坐标(mm)
static double currentFeedRate = DEFAULT_FEED_RATE; //当前速度(mm/min)

//激光状态
static int laserEnabled = 0;           //激光使能标志
static double currentSPower = 0.0;     //当前激光功率(0-1000)

//坐标模式
static int isAbsoluteMode = 1;         //1=绝对坐标G90, 0=相对坐标G91

//串口文件描述符
static int uart_fd = -1;

//时间戳
static struct timespec lastStatusTime; //上次状态报告时间

/* ================= 函数声明 ================= */
static void signal_handler(int sig);
static int uart_init(const char *device, speed_t baudrate);
static void uart_send(const char *msg);
static int uart_read_line(char *buf, int size, int timeout_ms);
static uint16_t mm_to_dac(double mm);
static void perform_move(double targetX, double targetY, double feedRateMmMin);
static void set_laser_power(double s);
static void process_gcode(const char *line);
static void send_status_report(void);
static int timespec_diff_ms(struct timespec *start, struct timespec *end);

/* ================= 工具函数 ================= */

//信号处理：Ctrl+C紧急停止
static void signal_handler(int sig) {
    if (sig == SIGINT) {
        printf("\n🛑 紧急停止！正在安全关闭系统...\n");
        running = 0;
    }
}

//计算时间差(毫秒)
static int timespec_diff_ms(struct timespec *start, struct timespec *end) {
    long sec_diff = end->tv_sec - start->tv_sec;
    long nsec_diff = end->tv_nsec - start->tv_nsec;
    return (int)(sec_diff * 1000 + nsec_diff / 1000000);
}

//初始化串口
static int uart_init(const char *device, speed_t baudrate) {
    int fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        perror("打开串口失败");
        return -1;
    }

    struct termios options;
    tcgetattr(fd, &options);

    //设置波特率
    cfsetispeed(&options, baudrate);
    cfsetospeed(&options, baudrate);

    //8N1：8数据位，无校验，1停止位
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    options.c_cflag |= CLOCAL | CREAD;

    //原始模式（非规范模式）
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    options.c_oflag &= ~OPOST;

    //超时设置：VMIN=0, VTIME=1（100ms超时）
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 1;

    tcflush(fd, TCIFLUSH);
    if (tcsetattr(fd, TCSANOW, &options) != 0) {
        perror("设置串口参数失败");
        close(fd);
        return -1;
    }

    return fd;
}

//发送字符串到串口
static void uart_send(const char *msg) {
    if (uart_fd < 0 || !msg) return;
    write(uart_fd, msg, strlen(msg));
}

//读取一行命令（阻塞，带超时）
static int uart_read_line(char *buf, int size, int timeout_ms) {
    if (!buf || size <= 0) return -1;

    int idx = 0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (running && idx < size - 1) {
        char c;
        int n = read(uart_fd, &c, 1);

        if (n > 0) {
            if (c == '\n' || c == '\r') {
                //忽略连续的换行符
                if (idx > 0) {
                    buf[idx] = '\0';
                    return idx;
                }
            } else {
                buf[idx++] = c;
            }
        } else {
            //检查超时
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (timespec_diff_ms(&start, &now) > timeout_ms) {
                break;
            }
            usleep(1000); //1ms休眠
        }
    }

    buf[idx] = '\0';
    return idx;
}

//毫米转DAC值
static uint16_t mm_to_dac(double mm) {
    double val = mm * BEILV;
#if ROTATE_180
    val = DAC_MAX - val;
#endif
    if (val < 0) val = 0;
    if (val > DAC_MAX) val = DAC_MAX;
    return (uint16_t)val;
}

//设置激光功率
static void set_laser_power(double s) {
    if (s < 0) s = 0;
    if (s > LASER_S_MAX) s = LASER_S_MAX;

    currentSPower = s;

    if (!laserEnabled) {
        pwm_laser_set_target(0); //激光未使能，强制0功率
    } else {
        int duty = (int)((s / LASER_S_MAX) * 1000); //0-1000映射
        pwm_laser_set_target(duty);
    }
}

//执行线性插补移动
static void perform_move(double targetX, double targetY, double feedRateMmMin) {
    double startX = currentX;
    double startY = currentY;
    double dx = targetX - startX;
    double dy = targetY - startY;
    double distance = sqrt(dx * dx + dy * dy);

    //距离太小，直接移动
    if (distance < STEP_NUM) {
        currentX = targetX;
        currentY = targetY;
        galvo_set_ab(mm_to_dac(currentX), mm_to_dac(currentY));
        return;
    }

    //计算步数
    int steps = (int)(distance / STEP_NUM);
    if (steps < 1) steps = 1;

    double stepDx = dx / steps;
    double stepDy = dy / steps;

    //计算每步时间(微秒)
    double feedRateSec = feedRateMmMin / 60.0;
    if (feedRateSec < 0.1) feedRateSec = 0.1;

    double totalTimeSec = distance / feedRateSec;
    double stepTimeUs = (totalTimeSec * 1000000.0) / steps;

    //限制最小时间（根据DAC响应速度）
    if (stepTimeUs < 100) stepTimeUs = 100; //100us = 10kpps

    //执行插补
    for (int i = 1; i <= steps && running; i++) {
        currentX += stepDx;
        currentY += stepDy;

        galvo_set_ab(mm_to_dac(currentX), mm_to_dac(currentY));

        usleep((useconds_t)stepTimeUs);
    }

    //修正最终位置
    currentX = targetX;
    currentY = targetY;
    galvo_set_ab(mm_to_dac(currentX), mm_to_dac(currentY));
}

//发送状态报告（Grbl格式）
static void send_status_report(void) {
    char buf[128];
    const char *state = "Idle"; //简化状态

    snprintf(buf, sizeof(buf), "<%s|MPos:%.3f,%.3f,0.000|FS:%.0f,%.0f>\r\n",
             state, currentX, currentY, currentFeedRate, currentSPower);

    uart_send(buf);
}

//解析并执行G代码命令
static void process_gcode(const char *line) {
    if (!line || strlen(line) == 0) return;

    //复制命令行并转换为大写
    char cmd[CMD_BUF_SIZE];
    strncpy(cmd, line, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';

    for (int i = 0; cmd[i]; i++) {
        cmd[i] = toupper(cmd[i]);
    }

    //处理实时命令
    if (strcmp(cmd, "?") == 0) {
        send_status_report();
        return; //不回复ok
    }

    //处理Grbl系统命令
    if (cmd[0] == '$') {
        if (strcmp(cmd, "$I") == 0) {
            uart_send("[VER:1.1f.Linux:BlackDandelion]\r\n");
            uart_send("[OPT:V,15,128]\r\n");
        } else if (strcmp(cmd, "$G") == 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "[GC:G0 G54 G17 G21 G%d G94 M%d T0 F%.0f S%.0f]\r\n",
                     isAbsoluteMode ? 90 : 91,
                     laserEnabled ? 3 : 5,
                     currentFeedRate,
                     currentSPower);
            uart_send(buf);
        }
        uart_send("ok\r\n");
        return;
    }

    //解析G代码参数
    double targetX = currentX, targetY = currentY;
    double feedRate = currentFeedRate;
    double sPower = currentSPower;
    int hasX = 0, hasY = 0, hasF = 0, hasS = 0;
    int gCode = -1, mCode = -1;

    char *p = cmd;
    while (*p) {
        char letter = *p++;
        if (!isalpha(letter)) continue;

        //读取数值
        char *endptr;
        double value = strtod(p, &endptr);
        if (endptr == p) continue; //无有效数字
        p = endptr;

        switch (letter) {
            case 'G': gCode = (int)value; break;
            case 'M': mCode = (int)value; break;
            case 'X': targetX = isAbsoluteMode ? value : (currentX + value); hasX = 1; break;
            case 'Y': targetY = isAbsoluteMode ? value : (currentY + value); hasY = 1; break;
            case 'F': feedRate = value; hasF = 1; break;
            case 'S': sPower = value; hasS = 1; break;
        }
    }

    //更新速度
    if (hasF) currentFeedRate = feedRate;

    //更新功率
    if (hasS) set_laser_power(sPower);

    //处理M代码（激光控制）
    if (mCode >= 0) {
        if (mCode == 3 || mCode == 4) {
            laserEnabled = 1;
            set_laser_power(currentSPower);
        } else if (mCode == 5) {
            laserEnabled = 0;
            set_laser_power(0);
        }
    }

    //处理G代码
    if (gCode >= 0) {
        switch (gCode) {
            case 0: //快速移动
                if (hasX || hasY) {
                    perform_move(targetX, targetY, G0_FEED_RATE);
                }
                break;

            case 1: //直线插补
                if (hasX || hasY) {
                    double speed = currentFeedRate;
                    if (speed <= 0) speed = DEFAULT_FEED_RATE;
                    perform_move(targetX, targetY, speed);
                }
                break;

            case 90: //绝对坐标模式
                isAbsoluteMode = 1;
                break;

            case 91: //相对坐标模式
                isAbsoluteMode = 0;
                break;

            case 92: //设置坐标原点
                currentX = 0.0;
                currentY = 0.0;
                galvo_set_ab(mm_to_dac(0), mm_to_dac(0));
                break;

            default:
                break; //未知G代码，忽略
        }
    }

    uart_send("ok\r\n");
}

/* ================= 主程序 ================= */

int main(int argc, char *argv[]) {
    printf("\n=== 激光打标机串口控制器 ===\n");
    printf("版本: Grbl 1.1f 兼容版 (Linux)\n");
    printf("串口: %s @ 115200\n\n", UART_DEVICE);

    //信号处理
    signal(SIGINT, signal_handler);

    //初始化振镜
    if (galvo_open() < 0) {
        fprintf(stderr, "错误: 无法打开振镜设备 /dev/galvo\n");
        return 1;
    }
    printf("✅ 振镜系统初始化成功\n");

    //初始化激光PWM
    if (pwm_laser_open() < 0) {
        fprintf(stderr, "错误: 无法打开激光设备 /dev/pwm_laser\n");
        galvo_close();
        return 1;
    }
    printf("✅ 激光系统初始化成功\n");

    //初始化串口
    uart_fd = uart_init(UART_DEVICE, UART_BAUDRATE);
    if (uart_fd < 0) {
        fprintf(stderr, "错误: 无法打开串口 %s\n", UART_DEVICE);
        pwm_laser_close();
        galvo_close();
        return 1;
    }
    printf("✅ 串口初始化成功\n\n");

    //振镜回中点
    galvo_set_ab(mm_to_dac(0), mm_to_dac(0));

    //发送启动消息
    sleep(1); //等待串口稳定
    uart_send("\r\n");
    uart_send("Grbl 1.1f ['$' for help]\r\n");

    printf("🚀 系统就绪，等待上位机命令...\n");
    printf("   按Ctrl+C紧急停止\n\n");

    clock_gettime(CLOCK_MONOTONIC, &lastStatusTime);

    //主循环
    char cmdBuf[CMD_BUF_SIZE];
    while (running) {
        //读取命令（100ms超时）
        int len = uart_read_line(cmdBuf, sizeof(cmdBuf), 100);

        if (len > 0) {
            //去除首尾空格
            char *start = cmdBuf;
            while (*start && isspace(*start)) start++;

            char *end = start + strlen(start) - 1;
            while (end > start && isspace(*end)) *end-- = '\0';

            if (strlen(start) > 0) {
                printf("RX: %s\n", start); //调试输出
                process_gcode(start);
            }
        }

        //定期状态报告（可选）
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (timespec_diff_ms(&lastStatusTime, &now) >= STATUS_INTERVAL_MS) {
            lastStatusTime = now;
            //send_status_report(); //取消自动状态报告，避免干扰
        }
    }

    //清理资源
    printf("\n🔴 正在安全关闭系统...\n");
    set_laser_power(0); //关闭激光
    galvo_set_ab(32768, 32768); //振镜回中点

    close(uart_fd);
    pwm_laser_close();
    galvo_close();

    printf("✅ 系统已安全关闭\n\n");
    return 0;
}
