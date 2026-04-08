# RK3566 开发板 (泰山派/瑞芯微) 生态环境与资源参数记录

## 基本系统信息

在执行探测命令后的核心指标反馈：
- **发行版名称**: Buildroot
- **具体版本**: `2018.02-rc3-00001-g4c7c9df6-dirty`
- **构建者/平台方案**: `lckfb@tspi-linux` / `rockchip_rk3566_defconfig`
- **内核版本**: Linux RK356X 4.19.232 #6 SMP aarch64 GNU/Linux

## 设备物理外设特性 (MIPI屏幕)

- **显示硬件**: 3.1寸 MIPI 长条竖屏 (型号: D310T9362V1)
- **分辨率参数**: 宽 480 像素 × 高 800 像素 (`hactive = 480; vactive = 800`)
- **触控芯片**: `CST128-A` (支持电容式多点触控交互)

## 关键功能模块侦测

*   **包管理器**: 无 (绝不考虑 `apt-get` 方案)。
*   **Web/浏览器内核支持**: 系统级阉割去除了 `libQt5WebEngine`，无法套网页壳子。
*   **图形引擎 (GUI)**: 系统自带完美适配底层 GPU 的 Qt5 运行环境（`eglfs` / `linuxfb`），原生刷屏性能炸裂。

## 交叉编译工具链 (Toolchain)

这是本工程日后编译和部署的最核心资产：

1.  **底层驱动与 C 代码编译器**:
    ```bash
    export ARCH=arm64 
    export CROSS_COMPILE=/home/lei/tspi_linux_sdk/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu- 
    ```
2.  **上层 Qt5 图形全站编译器 (qmake)**:
    原生配套的 `qmake` 直接存在于您早前编译的 SDK 输出目录中：
    `/home/lei/tspi_linux_sdk/buildroot/output/rockchip_rk3566/host/bin/qmake`
    *重要结论：使用该工具链生成的二进制应用，拷入板子可保证 0 依赖报错、直接全屏点亮 MIPI。*

## 平台技术路线方案 (LeiLaserUI 开发准则)

基于上述极度极客且受限的环境，制定下一阶段（原生控制端排版上位机）开发必遵守则：

1.  **产品形态**: 抛弃外部上位机，本板内直接开发一套名为 `LeiLaserUI` 的 Qt C++ 独立控制栈。
2.  **视觉设计死锁**: UI 所有窗体一经创建，强行定格为 `480x800`，去掉标题栏最大化按钮。按照现代竖向 APP 的逻辑，上放预览白板（触控拖拽图片），下方留足硕大的防误触操作按钮集群。
3.  **切片（CAM）算法引擎的纯手工再造**:
    我们绝不外联切片器。直接在程序体内使用 Qt 自带的数学类库 `QPainterPath`去提取任何 TTF 字体或是 SVG 矢量的轮廓 (`QPolygonF`)。
    将这成千上万个几何点序列化连接，按顺序当场转换成 `G0` 与 `G1` G-code 协议流。
4.  **底层通讯整合**:
    上述生成的巨量 G代码 不走网络，通过系统内创建伪终端 PTY（`/tmp/ttyGalvo`）灌入，或者干脆以 API 函数形式直接下塞给咱们辛辛苦苦调好的 `uart_controller.c` 底层解释器去驱动场镜振镜。
