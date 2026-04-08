# 🚀 [终极交底] 纯正交叉编译与泰山派系统内上屏实战点亮指南

在普通电脑写好的界面，如果放到嵌入式板子上跑，必须要过**“两道门”：正确的交叉编译、裸机（无桌面）渲染的环境参数！**

---

### 第一道门：使用宿主机的 Buildroot 神器进行交叉编译 🛠️

在您的 Ubuntu 宿主机上，我们不能用普通的 `gcc` 或者 `qmake` 去编译 `LeiLaserUI`，否则编出来的程序只能在 PC 电脑上运行！

我们必须使用您电脑里那个自带针对 aarch64 优化的“武器库”：
1. 打开终端，潜入咱们刚搭好的大本营：
   ```bash
   cd /home/lei/lei_laser/app
   ```
2. 为了防止刚才遇到的环境变量错误（找不到头文件等），我们将这套 SDK 产物路径直接**赋予顶级权限**加载到系统：
   ```bash
   export PATH=/home/lei/tspi_linux_sdk/buildroot/output/rockchip_rk3566/host/bin:$PATH
   ```
3. 下令让这把带有了 aarch64 和 LinuxFB 纯真基因的 `qmake` 去干活：
   ```bash
   # 为防止找不到配置设备描述文件，我们带上它
   qmake -spec /home/lei/tspi_linux_sdk/buildroot/output/rockchip_rk3566/host/mkspecs/devices/linux-buildroot-g++ LeiLaser.pro
   
   # 一切就绪后，直接多核拉满暴力编译它：
   make -j4
   ```
> 当命令行不再报错并且在您目录里出现了一个名叫 **`LeiLaserUI`** （体积很小，可能也就几兆或者几百 K）的没有后缀的绿油油的执行文件时，恭喜您跨平台武器锻造成功！

---

### 第二道门：投送与霸权上屏 (LinuxFB) 🖥️

将刚才诞生的那个精悍的 `LeiLaserUI` 文件使用网络复制或者 U 盘拷到您的泰山派板子中（比如放到 `/root/` 下）。

接着，我们必须**像个懂底层驱动的老手一样**去配置屏幕的环境：

因为这块板子并没有（我们也压根不需要）像 Windows 里面一样的“桌面窗口管理系统 X11”。我们需要强制咱们刚写的 C++ 程序，把像素直接通过 Linux 的缓冲驱动**直接往您那块 3.1 寸的物理屏幕（`/dev/fb0`）上按压进去**！

在有超级权限的开发板终端里运行以下指令：

```bash
# 给予程序最高执行权限
chmod +x /root/LeiLaserUI

# （重要）向 Qt 宣示我们在裸奔，直接调用底层硬件图形加速 EGLFS 或原图 FB 总线！
# 如果板子带 GPU 支持，请首选这个非常顺滑的引擎：
export QT_QPA_PLATFORM=eglfs

# 如果上面的报错，直接改用最传统的强制画面写入：
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0

# 在确保板子上没有任何闲杂程序占据屏幕后，启动发令：
./LeiLaserUI
```

随着这条命令按下，不要看命令框，请死死盯着您手边那块连接着排线的 3.1 寸小屏幕。那副纯正 C++ 手撕渲染出来的深蓝色巨无霸主控面板，将瞬间点亮它的像素海！🌟
