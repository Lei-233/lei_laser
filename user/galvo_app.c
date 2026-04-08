#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#include "galvo_hal.h"

static void generate_square_batch(struct galvo_point **out_points, uint32_t *out_count)
{
    const uint16_t MIN = 0, MAX = 65535;
    const uint16_t STEP = 80; 
    uint32_t count = (MAX - MIN) / STEP * 4;
    struct galvo_point *pts = malloc(count * sizeof(struct galvo_point));
    uint32_t idx = 0;
    
    uint16_t a = MIN, b = MIN;
    
    while (a < MAX && idx < count) {
        pts[idx++] = (struct galvo_point){a, b, 800, 100}; 
        a += STEP;
    }
    a = MAX;
    while (b < MAX && idx < count) {
        pts[idx++] = (struct galvo_point){a, b, 800, 100};
        b += STEP;
    }
    b = MAX;
    while (a > MIN + STEP && idx < count) {
        pts[idx++] = (struct galvo_point){a, b, 800, 100};
        a -= STEP;
    }
    a = MIN;
    while (b > MIN + STEP && idx < count) {
        pts[idx++] = (struct galvo_point){a, b, 800, 100};
        b -= STEP;
    }
    
    *out_points = pts;
    *out_count = idx;
}

static void generate_circle_batch(struct galvo_point **out_points, uint32_t *out_count)
{
    const int CENTER = 32768;
    const int R = 24000;
    const int N = 2000; 
    
    struct galvo_point *pts = malloc(N * sizeof(struct galvo_point));
    
    for (int i = 0; i < N; i++) {
        float theta = 2.0f * 3.1415926f * (float)i / (float)N;
        int a = CENTER + (int)(R * sinf(theta));
        int b = CENTER + (int)(R * cosf(theta));
        
        if (a < 0) a = 0; if (a > 65535) a = 65535;
        if (b < 0) b = 0; if (b > 65535) b = 65535;
        
        pts[i] = (struct galvo_point){ (uint16_t)a, (uint16_t)b, 500, 50 }; 
    }
    
    *out_points = pts;
    *out_count = N;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s <mode>\n", argv[0]);
        printf("  0: square\n");
        printf("  1: circle\n");
        return -1;
    }

    int mode = atoi(argv[1]);
    struct galvo_point *points = NULL;
    uint32_t count = 0;
    
    if (mode == 0) {
        generate_square_batch(&points, &count);
    } else if (mode == 1) {
        generate_circle_batch(&points, &count);
    } else {
        printf("Invalid mode!\n");
        return -1;
    }

    if (galvo_open() < 0) {
        perror("galvo_open");
        free(points);
        return -1;
    }
    
    printf("Generated %u points in application heap space. Sending via ioctl...\n", count);
    
    /* 系统调用：安全隔离且一口气透传所有结构体 */
    if (galvo_send_batch(points, count) < 0) {
        perror("galvo_send_batch");
    } else {
        printf("All points successfully offloaded to kernel! App is now free.\n");
        printf("Kernel hrtimer will asynchronously finish the high-precision processing.\n");
    }

    galvo_close();
    free(points);
    return 0;
}
