#!/bin/bash
set -e

IMG_FILE="t113-debian.img"
MNT_DIR="./mnt"

# 检查镜像文件
if [ ! -f "$IMG_FILE" ]; then
    echo "Error: $IMG_FILE not found. Run 'make build-img' first."
    exit 1
fi

# 设置 loop 设备
echo "Setting up loop device..."
LOOP_DEV=$(sudo losetup -f --show -P "$IMG_FILE")
ROOTFS_LOOP="${LOOP_DEV}p2"

# 等待分区设备出现
for i in {1..5}; do
    if [ -b "$ROOTFS_LOOP" ]; then
        echo "Found partition: $ROOTFS_LOOP"
        break
    fi
    sleep 1
done

if [ ! -b "$ROOTFS_LOOP" ]; then
    echo "Error: Partition device $ROOTFS_LOOP not found"
    sudo losetup -d "$LOOP_DEV"
    exit 1
fi

# 挂载并准备环境
sudo mkdir -p "$MNT_DIR"
sudo mount "$ROOTFS_LOOP" "$MNT_DIR"

# 创建必要的挂载点
sudo mkdir -p "$MNT_DIR/tmp" "$MNT_DIR/dev" "$MNT_DIR/proc" "$MNT_DIR/sys"
sudo chmod 1777 "$MNT_DIR/tmp"  # 设置 tmp 目录权限为 1777

# 挂载必要的虚拟文件系统
sudo mount --bind /dev "$MNT_DIR/dev"
sudo mount --bind /proc "$MNT_DIR/proc"
sudo mount --bind /sys "$MNT_DIR/sys"

echo "Entering chroot environment (exit to continue)..."
sudo LC_ALL=C LANGUAGE=C LANG=C chroot "$MNT_DIR"

# 清理
echo "Syncing and cleaning up..."
sync
sudo umount "$MNT_DIR/sys" "$MNT_DIR/proc" "$MNT_DIR/dev" 2>/dev/null || true
sudo umount "$MNT_DIR"
sudo losetup -d "$LOOP_DEV"
sudo rmdir "$MNT_DIR" 2>/dev/null || true

echo "Chroot session completed."