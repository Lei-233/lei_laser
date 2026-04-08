cmake_minimum_required(VERSION 3.28)

# 配置交叉编译器
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER "/home/lei/tspi_linux_sdk/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc")

project(laser_3 C)

# 编译选项
add_compile_options(-Wall -O2)

# ---------- pwm_app ----------
add_executable(pwm_app pwm_app.c pwm_laser_hal.c)

# ---------- galvo_app ----------
add_executable(galvo_app galvo_app.c galvo_hal.c)
target_link_libraries(galvo_app m)

# ---------- uart_controller ----------
add_executable(uart_controller uart_controller.c galvo_hal.c pwm_laser_hal.c)
target_link_libraries(uart_controller m)