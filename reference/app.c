#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <stdbool.h>

#define MCP4822_DEV_PATH "/dev/MCP4822"

struct mcp4822_xy {
    uint16_t a; // Y
    uint16_t b; // X
} __attribute__((packed));

static int write_xy(int fd, uint16_t a, uint16_t b)
{
    struct mcp4822_xy xy = { a, b };
    ssize_t n = write(fd, &xy, sizeof(xy));
    return (n == (ssize_t)sizeof(xy)) ? 0 : -1;
}

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

/* ---------------- 你原来的形状 ---------------- */

/* 你的映射：A=纵轴(Y), B=横轴(X), 0..4095 */
static void shape_square_step(uint16_t *A, uint16_t *B)
{
    const uint16_t MIN = 0, MAX = 4095;
    const uint16_t STEP = 100;

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
    const int CENTER = 2048;
    const int R = 1500;

    const int N = 100;
    static int idx = 0;

    float theta = 2.0f * 3.1415926f * (float)idx / (float)N;
    int a = CENTER + (int)(R * sinf(theta));
    int b = CENTER + (int)(R * cosf(theta));

    if (a < 0) a = 0; if (a > 4095) a = 4095;
    if (b < 0) b = 0; if (b > 4095) b = 4095;

    *A = (uint16_t)a;
    *B = (uint16_t)b;

    idx++; if (idx >= N) idx = 0;
}

#define PENTA_N_PER_EDGE   60
#define PENTA_TOTAL_POINTS (5 * PENTA_N_PER_EDGE)

static void shape_pentagram_step(uint16_t *A, uint16_t *B)
{
    const int CENTER = 2048;
    const int R = 1500;

    static int inited = 0;
    static uint16_t tableA[PENTA_TOTAL_POINTS];
    static uint16_t tableB[PENTA_TOTAL_POINTS];
    static int idx = 0;

    if (!inited) {
        int VA[5], VB[5];
        for (int i = 0; i < 5; i++) {
            float theta = (-90.0f + 72.0f * (float)i) * 3.1415926f / 180.0f;
            VA[i] = CENTER + (int)(R * sinf(theta));
            VB[i] = CENTER + (int)(R * cosf(theta));
        }

        const int order[6] = {0, 2, 4, 1, 3, 0};
        int p = 0;

        for (int e = 0; e < 5; e++) {
            int a0 = VA[order[e]],   b0 = VB[order[e]];
            int a1 = VA[order[e+1]], b1 = VB[order[e+1]];

            for (int k = 0; k < PENTA_N_PER_EDGE; k++) {
                int a = a0 + (a1 - a0) * k / PENTA_N_PER_EDGE;
                int b = b0 + (b1 - b0) * k / PENTA_N_PER_EDGE;

                if (a < 0) a = 0; if (a > 4095) a = 4095;
                if (b < 0) b = 0; if (b > 4095) b = 4095;

                tableA[p] = (uint16_t)a;
                tableB[p] = (uint16_t)b;
                p++;
            }
        }

        inited = 1;
    }

    *A = tableA[idx];
    *B = tableB[idx];

    idx++;
    if (idx >= PENTA_TOTAL_POINTS) idx = 0;
}

/* ---------------- ILDA .ILD 读取与播放 ---------------- */

/* ILDA header 32 bytes (big-endian fields) */
struct ilda_header {
    char     magic[4];      /* "ILDA" */
    uint8_t  reserved1[3];
    uint8_t  format;        /* 0,1,2,3,4,5... */
    char     name[8];
    char     company[8];
    uint16_t num_records;   /* big-endian */
    uint16_t frame_number;  /* big-endian */
    uint16_t total_frames;  /* big-endian */
    uint8_t  projector;
    uint8_t  reserved2;
} __attribute__((packed));

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int16_t be16s(const uint8_t *p)
{
    return (int16_t)((p[0] << 8) | p[1]);
}

/* 播放时我们只关心 X/Y + blanking(可选)，忽略颜色 */
struct ilda_point {
    int16_t x;      /* ILDA X: -32768..32767 */
    int16_t y;      /* ILDA Y */
    uint8_t status; /* bit6 常用于 blanking（不同文件略有差异） */
};

/*
 * ILDA坐标映射到 DAC：
 * dac = center + (coord/32768)*amp
 * - center 默认 2048
 * - amp 决定输出幅度（别打满 0..4095，留点余量更稳）
 */
static inline uint16_t map_ilda_to_dac(int16_t coord, int center, int amp)
{
    float norm = (float)coord / 32768.0f;      // approx [-1, 1)
    int v = center + (int)lroundf(norm * (float)amp);
    if (v < 0) v = 0;
    if (v > 4095) v = 4095;
    return (uint16_t)v;
}

/* 读取整个 ILDA 文件：把支持的点格式（0/1/4/5）全部展开到数组 */
static int load_ild(const char *path, struct ilda_point **out_pts, size_t *out_n)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror("fopen .ild failed");
        return -1;
    }

    struct ilda_point *pts = NULL;
    size_t npts = 0, cap = 0;

    while (1) {
        struct ilda_header h;
        size_t nr = fread(&h, 1, sizeof(h), fp);
        if (nr == 0) break;                    // EOF
        if (nr != sizeof(h)) {
            fprintf(stderr, "ILD: short header read\n");
            goto fail;
        }
        if (memcmp(h.magic, "ILDA", 4) != 0) {
            fprintf(stderr, "ILD: bad magic, not ILDA\n");
            goto fail;
        }

        uint16_t num = be16((uint8_t *)&h.num_records);
        uint8_t fmt = h.format;

        /* num==0 常用作结束块（也可能还有别的块，稳妥起见：继续解析但会跳过0记录） */
        if (num == 0) {
            continue;
        }

        /* 支持：0(3D indexed), 1(2D indexed), 4(3D truecolor), 5(2D truecolor)
           其他格式（如palette=2）直接跳过 */
        size_t rec_size = 0;
        bool has_z = false;
        bool supported = true;

        switch (fmt) {
        case 0: rec_size = 8;  has_z = true;  break; // x y z status color
        case 1: rec_size = 6;  has_z = false; break; // x y status color
        case 4: rec_size = 10; has_z = true;  break; // x y z status r g b
        case 5: rec_size = 8;  has_z = false; break; // x y status r g b
        default:
            supported = false;
            break;
        }

        if (!supported) {
            /* 跳过未知记录 */
            long skip = (long)num * (long)(fmt == 2 ? 3 : 0);
            if (skip > 0) fseek(fp, skip, SEEK_CUR);
            else {
                /* 不知道尺寸就只能保守退出，避免错位解析 */
                fprintf(stderr, "ILD: unsupported format=%u (can't safely skip)\n", fmt);
                goto fail;
            }
            continue;
        }

        for (uint16_t i = 0; i < num; i++) {
            uint8_t rec[16];
            if (fread(rec, 1, rec_size, fp) != rec_size) {
                fprintf(stderr, "ILD: short record read\n");
                goto fail;
            }

            int16_t x = be16s(&rec[0]);
            int16_t y = be16s(&rec[2]);
            uint8_t status;

            if (has_z) {
                /* z 在 rec[4..5]，我们忽略 */
                status = rec[6];
            } else {
                status = rec[4];
            }

            if (npts == cap) {
                size_t newcap = (cap == 0) ? 4096 : cap * 2;
                struct ilda_point *np = realloc(pts, newcap * sizeof(*pts));
                if (!np) {
                    perror("realloc");
                    goto fail;
                }
                pts = np;
                cap = newcap;
            }
            pts[npts++] = (struct ilda_point){ .x = x, .y = y, .status = status };
        }
    }

    fclose(fp);
    *out_pts = pts;
    *out_n = npts;
    return 0;

fail:
    fclose(fp);
    free(pts);
    return -1;
}

/* 可选：对“跳跃很大”的段做简单插值，减轻抖动/断线感 */
static void output_with_interp(int fd,
                               uint16_t Ay, uint16_t Bx,
                               uint16_t Ay2, uint16_t Bx2,
                               int max_step,
                               long us_per_point)
{
    int dy = (int)Ay2 - (int)Ay;
    int dx = (int)Bx2 - (int)Bx;
    int ady = dy < 0 ? -dy : dy;
    int adx = dx < 0 ? -dx : dx;
    int dist = (ady > adx) ? ady : adx;

    int steps = dist / max_step;
    if (steps < 1) steps = 1;

    for (int i = 1; i <= steps; i++) {
        uint16_t a = (uint16_t)((int)Ay + dy * i / steps);
        uint16_t b = (uint16_t)((int)Bx + dx * i / steps);
        if (write_xy(fd, a, b) != 0) return;
        sleep_us_abs(us_per_point);
    }
}

static void usage(const char *p)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s <mode> [args...]\n"
        "Modes:\n"
        "  0 : square\n"
        "  1 : circle\n"
        "  2 : pentagram\n"
        "  3 : play ILDA .ild  -> %s 3 output.ild [pps] [amp] [flipx] [flipy]\n"
        "      pps  : points per second, default 10000\n"
        "      amp  : amplitude around center(2048), default 1800 (range 0..2047)\n"
        "      flipx/flipy : 0 or 1, default 0\n"
        , p, p);
}

int main(int argc, char *argv[])
{
    int fd = open(MCP4822_DEV_PATH, O_RDWR);
    if (fd < 0) {
        perror("open MCP4822 device failed");
        return -1;
    }

    if (argc < 2) {
        usage(argv[0]);
        close(fd);
        return -1;
    }

    int mode = atoi(argv[1]);

    /* 默认点速：10kpps（说明书也建议一般10Kpps）:contentReference[oaicite:1]{index=1} */
    long pps = 10000;
    long us_per_point = 1000000L / pps;

    uint16_t A = 0, B = 0;

    if (mode == 3) {
        
        const char *ild_path = (argc >= 3) ? argv[2] : "output.ild";
        if (argc >= 4) {
            pps = atol(argv[3]);
            if (pps <= 0) pps = 10000;
        }
        us_per_point = 1000000L / pps;

        int amp = 1800;              // 建议别打满，留余量更稳
        int flipx = 0, flipy = 1;

        if (argc >= 5) amp = atoi(argv[4]);
        if (amp < 0) amp = 0;
        if (amp > 2047) amp = 2047;

        if (argc >= 6) flipx = atoi(argv[5]) ? 1 : 0;
        if (argc >= 7) flipy = atoi(argv[6]) ? 1 : 0;

        struct ilda_point *pts = NULL;
        size_t npts = 0;
        if (load_ild(ild_path, &pts, &npts) != 0 || npts == 0) {
            fprintf(stderr, "Failed to load ILD or no points: %s\n", ild_path);
            close(fd);
            return -1;
        }

        fprintf(stderr, "Loaded %zu points from %s, pps=%ld, us=%ld, amp=%d, flipx=%d, flipy=%d\n",
                npts, ild_path, pps, us_per_point, amp, flipx, flipy);

        /* 播放循环 */
        size_t idx = 0;
        uint16_t prevA = 2048, prevB = 2048;

        while (1) {
            struct ilda_point p = pts[idx];

            int16_t x = p.x;
            int16_t y = p.y;

            if (flipx) x = (int16_t)(-x);
            if (flipy) y = (int16_t)(-y);

            /* 你映射：A=Y, B=X */
            uint16_t curA = map_ilda_to_dac(y, 2048, amp);
            uint16_t curB = map_ilda_to_dac(x, 2048, amp);

            /* 简单插值：大跳跃时分段输出，避免突然甩镜（可按需关掉） */
            output_with_interp(fd, prevA, prevB, curA, curB, 120, us_per_point);

            prevA = curA;
            prevB = curB;

            idx++;
            if (idx >= npts) idx = 0;
        }

        /* 不会走到这里 */
        free(pts);
    }

    /* 你原来的几何图形模式 */
    while (1) {
        if (mode == 0) {
            shape_square_step(&A, &B);
        } else if (mode == 1) {
            shape_circle_step(&A, &B);
        } else if (mode == 2) {
            shape_pentagram_step(&A, &B);
        } else {
            usage(argv[0]);
            break;
        }

        if (write_xy(fd, A, B) != 0) {
            perror("write_xy failed");
            break;
        }

        /* 这里对几何图形也建议别用 20us（50Kpps），可按你的振镜调到 100us(10Kpps) */
        sleep_us_abs(100);
    }

    close(fd);
    return 0;
}
