# T113-S3/4板卡Debian系统全套构建支持

## 介绍

本储存库涵盖了bootloader(awboot)、主线linux-6.1、debian根文件系统构建脚本、一键制作镜像与下载脚本，以及板卡的硬件设计以及外壳3D文件。

适用的板卡是我自己搓的T113 IoT Station：

![板卡](images/board.png)
![组装](images/demonstrate.gif)

**Bilibili：[自制比掌心还小的Linux电脑：从高中生玩具到全能终端的进化](https://www.bilibili.com/video/BV1PycHzdEiS)**

板子带外壳尺寸54\*41\*13，屏幕尺寸1.8寸，内置wifi蓝牙、麦克风扬声器等。相关文件在hardware文件夹中。

得益于RNDIS的USB局域网，日常调试应用可以直接一线通（SSH、桌面串流程序等）。

图示的桌面串流程序可实现将板卡作为windows的副屏使用（win11 vdd虚拟屏幕实时截屏+RNDIS局域网串流，通信架构为TCP+ZeroMQ），大部分是AI写的，没什么参考价值，仅作留档。

本项目其实2023年初就基本完成了（当时只上传了内核储存库），一直没有继续整理资料的打算，直到这几天闲下来才继续搞

## 构建

### 克隆储存库与子模块

```
git clone --recursive https://github.com/fEndman/t113-iot-build-debian.git
cd t113-iot-build-debian
```

编译环境请自行配置，交叉编译使用arm-linux-gnueabihf，已验证gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf（注意linux6对gcc有版本要求）

按顺序分别进入以下文件夹参考内部README指引构建各模块：

### awboot

```
cd awboot
make
cd ..
```

### linux-6.1-t113

```
cd linux-6.1-t113
make t113_iot_station_defconfig
make -j$(nproc)
cd ..
```

### debian

```
cd debian
./build.sh
cd ..
```

### 烧录镜像

最后进入tools文件夹，根据其内指引完成最终tf卡镜像构建与烧录。

```
cd tools
make build-img
make flash-sd DEV_FILE=/dev/sdX
```

```
# 通过ssh为一个已运行的板卡远程更新内核和设备树
make sftp-download-core
```
