# 交叉编译器（如果你是在 x86 上给 ARM 板编译）
CC := /home/lei/tspi_linux_sdk/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc

# 编译选项
CFLAGS := -Wall -O2

# 目标程序
TARGETS := pwm_app galvo_app uart_controller

all: $(TARGETS)

# ---------- pwm 激光功率控制程序 ----------
pwm_app: pwm_app.o pwm_laser_hal.o
	$(CC) $(CFLAGS) -o $@ $^

# ---------- galvo 振镜控制程序 ----------
galvo_app: galvo_app.o galvo_hal.o -lm
	$(CC) $(CFLAGS) -o $@ galvo_app.o galvo_hal.o -lm

# ---------- 串口控制器（整合振镜+激光+Grbl协议） ----------
uart_controller: uart_controller.o galvo_hal.o pwm_laser_hal.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

# ---------- 通用规则 ----------
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(TARGETS)


