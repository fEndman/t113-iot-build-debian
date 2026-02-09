# 辅助工具

一系列工具，用于构建镜像、下载镜像、热更新内核与设备树

```
make help
```

```
make build-img
make flash-sd DEV_FILE=/dev/sdX
# make clean-img
```

```
# 通过ssh为一个正常运行的板卡远程更新内核和设备树
make sftp-download-core
```
