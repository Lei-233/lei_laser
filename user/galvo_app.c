#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#include "galvo_hal.h"

/* 精确绝对时间睡眠 - 消除累积误差，防止图像闪烁 */
static void sleep_us_abs(long us)
{
    static struct timespec next = {0};
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (next.tv_sec == 0 && next.tv_nsec == 0)
        next = now;

    next.tv_nsec += us * 1000;
    while (next.tv_nsec >= 1000000000L) {
        next.tv_nsec -= 1000000000L;
        next.tv_sec += 1;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
}

/* ---------- DAC8562 16位范围算法 ---------- */

static void shape_square_step(uint16_t *A, uint16_t *B)
{
    const uint16_t MIN = 0, MAX = 65535;
    const uint16_t STEP = 1600;  /* 16倍于原12位的100 */
    static uint8_t edge = 0;

    switch (edge) {
    case 0:
        *B = MIN;
        if (*A + STEP >= MAX) { *A = MAX; edge = 1; }
        else *A += STEP;
        break;
    case 1:
        *A = MAX;
        if (*B + STEP >= MAX) { *B = MAX; edge = 2; }
        else *B += STEP;
        break;
    case 2:
        *B = MAX;
        if (*A <= STEP) { *A = MIN; edge = 3; }
        else *A -= STEP;
        break;
    default:
        *A = MIN;
        if (*B <= STEP) { *B = MIN; edge = 0; }
        else *B -= STEP;
        break;
    }
}

static void shape_circle_step(uint16_t *A, uint16_t *B)
{
    const int CENTER = 32768;  /* 16位中点 */
    const int R = 24000;       /* 约覆盖75%范围 */
    const int N = 100;
    static int idx = 0;

    float theta = 2.0f * 3.1415926f * (float)idx / (float)N;
    int a = CENTER + (int)(R * sinf(theta));
    int b = CENTER + (int)(R * cosf(theta));

    if (a < 0) a = 0;
    if (a > 65535) a = 65535;
    if (b < 0) b = 0;
    if (b > 65535) b = 65535;

    *A = (uint16_t)a;
    *B = (uint16_t)b;

    idx++; if (idx >= N) idx = 0;
}

/* ---------- 主程序 ---------- */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s <mode>\n", argv[0]);
        printf("  0: square\n");
        printf("  1: circle\n");
        return -1;
    }

    int mode = atoi(argv[1]);
    uint16_t A = 32768, B = 32768;  /* 16位中点启动 */

    if (galvo_open() < 0) {
        perror("galvo_open");
        return -1;
    }

    while (1) {
        if (mode == 0) {
            shape_square_step(&A, &B);
        } else if (mode == 1) {
            shape_circle_step(&A, &B);
        } else {
            break;
        }

        if (galvo_set_ab(A, B) < 0) {
            perror("galvo_set_ab");
            break;
        }

        /* 精确10Kpps：100us间隔，使用绝对时间避免累积误差 */
        sleep_us_abs(100);
    }

    galvo_close();
    return 0;
}
