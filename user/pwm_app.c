/* PWM激光功率测试工具 - 用于验证激光功率控制 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include "pwm_laser_hal.h"

static volatile int running = 1; //信号处理标志

static void signal_handler(int sig) { running = 0; } //Ctrl+C退出

static void print_help(const char *prog) {
    printf("\n=== PWM激光功率测试工具 ===\n");
    printf("用法: %s <模式> [参数]\n\n", prog);
    printf("模式:\n");
    printf("  off              - 关闭激光 (0%%功率)\n");
    printf("  on <power>       - 固定功率输出 (1-1000, 对应0.1%%-100%%)\n");
    printf("  fade <min> <max> <speed>\n");
    printf("                   - 渐变测试 (淡入淡出循环)\n");
    printf("  pulse <power> <on_ms> <off_ms>\n");
    printf("                   - 脉冲闪烁测试\n");
    printf("  step <start> <end> <step> <delay_ms>\n");
    printf("                   - 步进功率测试\n\n");
    printf("示例:\n");
    printf("  %s off                    # 关闭激光\n", prog);
    printf("  %s on 100                 # 10%%功率连续输出\n", prog);
    printf("  %s on 500                 # 50%%功率连续输出\n", prog);
    printf("  %s fade 0 1000 10         # 0-100%%渐变，步长1%%\n", prog);
    printf("  %s pulse 500 100 100      # 50%%功率，闪烁100ms开/100ms关\n", prog);
    printf("  %s step 0 1000 50 500     # 0-100%%步进，步长5%%，停500ms\n\n", prog);
    printf("安全提示:\n");
    printf("  ⚠️  激光危险！测试时必须佩戴激光防护眼镜\n");
    printf("  ⚠️  首次测试建议从低功率（10-50）开始\n");
    printf("  ⚠️  确保激光出射口无人员和易燃物\n");
    printf("  ⚠️  按Ctrl+C立即停止并关闭激光\n\n");
    printf("功率参数说明:\n");
    printf("  范围: 0-1000 (对应 0%%-100%%)\n");
    printf("  推荐起始测试功率: 50-100 (5%%-10%%)\n");
    printf("  PWM频率: 1KHz (1ms周期)\n\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }

    if (pwm_laser_open() < 0) {
        perror("错误: 无法打开 /dev/pwm_laser");
        printf("提示: 请确保 pwm_laser.ko 模块已加载\n");
        return 1;
    }

    signal(SIGINT, signal_handler); //注册Ctrl+C处理

    const char *mode = argv[1];

    /* 模式1: 关闭激光 */
    if (strcmp(mode, "off") == 0) {
        printf("\n🔴 关闭激光...\n");
        pwm_laser_set_target(0);
        printf("✅ 激光已关闭 (PWM占空比: 0%%)\n\n");
    }

    /* 模式2: 固定功率输出 */
    else if (strcmp(mode, "on") == 0) {
        if (argc < 3) {
            fprintf(stderr, "错误: 缺少功率参数\n");
            fprintf(stderr, "用法: %s on <功率 0-1000>\n", argv[0]);
            pwm_laser_close();
            return 1;
        }

        int power = atoi(argv[2]);
        if (power < 0 || power > 1000) {
            fprintf(stderr, "错误: 功率超出范围 (0-1000)\n");
            pwm_laser_close();
            return 1;
        }

        printf("\n🟢 开启激光\n");
        printf("功率设置: %d/1000 (%.1f%%)\n", power, power / 10.0f);
        printf("PWM占空比: %.1f%%\n", power / 10.0f);
        printf("按Ctrl+C停止并关闭激光...\n\n");

        if (pwm_laser_set_target(power) < 0) {
            perror("pwm_laser_set_target失败");
            pwm_laser_close();
            return 1;
        }

        printf("⚠️  激光已开启！保持此功率，按Ctrl+C关闭\n");

        //等待Ctrl+C
        while (running) {
            sleep(1);
        }

        printf("\n\n🔴 正在关闭激光...\n");
        pwm_laser_set_target(0);
        printf("✅ 激光已安全关闭\n");
    }

    /* 模式3: 渐变测试 */
    else if (strcmp(mode, "fade") == 0) {
        if (argc < 5) {
            fprintf(stderr, "错误: 参数不足\n");
            fprintf(stderr, "用法: %s fade <最小功率> <最大功率> <步长>\n", argv[0]);
            pwm_laser_close();
            return 1;
        }

        int min_power = atoi(argv[2]);
        int max_power = atoi(argv[3]);
        int step = atoi(argv[4]);

        if (min_power < 0 || max_power > 1000 || min_power >= max_power) {
            fprintf(stderr, "错误: 功率范围无效\n");
            pwm_laser_close();
            return 1;
        }

        printf("\n🌈 渐变功率测试\n");
        printf("范围: %d - %d (%.1f%% - %.1f%%)\n",
               min_power, max_power, min_power / 10.0f, max_power / 10.0f);
        printf("步长: %d (%.1f%%)\n", step, step / 10.0f);
        printf("按Ctrl+C停止...\n\n");

        int power = min_power;
        int direction = 1; //1=递增, -1=递减

        while (running) {
            pwm_laser_set_target(power);
            printf("\r功率: %4d/1000 (%.1f%%)  ", power, power / 10.0f);
            fflush(stdout);

            if (direction == 1) {
                power += step;
                if (power >= max_power) {
                    power = max_power;
                    direction = -1;
                }
            } else {
                power -= step;
                if (power <= min_power) {
                    power = min_power;
                    direction = 1;
                }
            }

            usleep(10000); //10ms延迟，100Hz更新
        }

        printf("\n\n🔴 正在关闭激光...\n");
        pwm_laser_set_target(0);
        printf("✅ 激光已安全关闭\n");
    }

    /* 模式4: 脉冲闪烁测试 */
    else if (strcmp(mode, "pulse") == 0) {
        if (argc < 5) {
            fprintf(stderr, "错误: 参数不足\n");
            fprintf(stderr, "用法: %s pulse <功率> <开启时间ms> <关闭时间ms>\n", argv[0]);
            pwm_laser_close();
            return 1;
        }

        int power = atoi(argv[2]);
        int on_ms = atoi(argv[3]);
        int off_ms = atoi(argv[4]);

        if (power < 0 || power > 1000) {
            fprintf(stderr, "错误: 功率超出范围 (0-1000)\n");
            pwm_laser_close();
            return 1;
        }

        printf("\n💡 脉冲闪烁测试\n");
        printf("功率: %d/1000 (%.1f%%)\n", power, power / 10.0f);
        printf("时序: %dms ON / %dms OFF\n", on_ms, off_ms);
        printf("频率: %.2f Hz\n", 1000.0f / (on_ms + off_ms));
        printf("按Ctrl+C停止...\n\n");

        int count = 0;
        while (running) {
            //开启
            pwm_laser_set_target(power);
            printf("\r[%4d] 🟢 ON  (%.1f%%)  ", ++count, power / 10.0f);
            fflush(stdout);
            usleep(on_ms * 1000);

            if (!running) break;

            //关闭
            pwm_laser_set_target(0);
            printf("\r[%4d] ⚫ OFF          ", count);
            fflush(stdout);
            usleep(off_ms * 1000);
        }

        printf("\n\n🔴 正在关闭激光...\n");
        pwm_laser_set_target(0);
        printf("✅ 激光已安全关闭\n");
    }

    /* 模式5: 步进测试 */
    else if (strcmp(mode, "step") == 0) {
        if (argc < 6) {
            fprintf(stderr, "错误: 参数不足\n");
            fprintf(stderr, "用法: %s step <起始> <结束> <步长> <延迟ms>\n", argv[0]);
            pwm_laser_close();
            return 1;
        }

        int start = atoi(argv[2]);
        int end = atoi(argv[3]);
        int step_size = atoi(argv[4]);
        int delay_ms = atoi(argv[5]);

        if (start < 0 || end > 1000 || start >= end) {
            fprintf(stderr, "错误: 范围无效\n");
            pwm_laser_close();
            return 1;
        }

        printf("\n📊 步进功率测试\n");
        printf("范围: %d - %d (%.1f%% - %.1f%%)\n",
               start, end, start / 10.0f, end / 10.0f);
        printf("步长: %d (%.1f%%)\n", step_size, step_size / 10.0f);
        printf("每步停留: %d ms\n", delay_ms);
        printf("按Ctrl+C停止...\n\n");

        int power = start;
        int step_count = 0;

        while (power <= end && running) {
            pwm_laser_set_target(power);
            printf("[步骤 %3d] 功率: %4d/1000 (%.1f%%) - 停留%dms\n",
                   ++step_count, power, power / 10.0f, delay_ms);

            usleep(delay_ms * 1000);
            power += step_size;
        }

        printf("\n🔴 正在关闭激光...\n");
        pwm_laser_set_target(0);
        printf("✅ 激光已安全关闭\n");
        printf("总步数: %d\n", step_count);
    }

    else {
        fprintf(stderr, "错误: 未知模式 '%s'\n", mode);
        print_help(argv[0]);
        pwm_laser_close();
        return 1;
    }

    pwm_laser_close();
    return 0;
}
