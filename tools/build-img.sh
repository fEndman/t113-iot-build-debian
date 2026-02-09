#!/bin/bash
set -e  # 遇到错误立即退出

# ===== 清理函数 =====
cleanup() {
    echo "Cleaning up..."
    # 卸载挂载点
    sudo umount "${MNT_DIR}/boot" "${MNT_DIR}/rootfs" 2>/dev/null || true
    # 删除目录
    rmdir "${MNT_DIR}/boot" "${MNT_DIR}/rootfs" 2>/dev/null || true
    # 释放 loop 设备
    if [ -n "${LOOP_DEV+x}" ]; then
        sudo losetup -d "${LOOP_DEV}" 2>/dev/null || true
    fi
}

# 设置退出时执行清理
trap cleanup EXIT

# 脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

PROJECT=".."
MNT_DIR="./mnt"

AWBOOT_FILE="${PROJECT}/awboot/awboot-boot-sd.bin"
DTB_FILE="${PROJECT}/linux-6.1-t113/arch/arm/boot/dts/sun8i-t113-iot-station.dtb"
KERNEL_FILE="${PROJECT}/linux-6.1-t113/arch/arm/boot/zImage"
DEBIAN_ROOTFS_FILES="${PROJECT}/debian/rootfs"

# ===== 内核源码路径（用于生成 modules.dep 等元数据） =====
LINUX_SRC="${PROJECT}/linux-6.1-t113"
KERNEL_VERSION="6.1.0+"  # 与内核版本匹配
MODULES_TARGET_DIR="/lib/modules/${KERNEL_VERSION}"

# ===== 旧的单个模块列表（保留作为补充） =====
MODULES_FILES=(
    "${PROJECT}/linux-6.1-t113/drivers/staging/rtl8723bs/r8723bs.ko"
    "${PROJECT}/linux-6.1-t113/drivers/bluetooth/hci_uart.ko"
    "${PROJECT}/linux-6.1-t113/lib/crypto/libarc4.ko"
    "${PROJECT}/linux-6.1-t113/net/wireless/cfg80211.ko"
    "${PROJECT}/linux-6.1-t113/net/mac80211/mac80211.ko"
    "${PROJECT}/linux-6.1-t113/drivers/bluetooth/btrtl.ko"
    "${PROJECT}/linux-6.1-t113/drivers/block/zram/zram.ko"
    "${PROJECT}/linux-6.1-t113/mm/zsmalloc.ko"
    # "${PROJECT}/linux-6.1-t113/drivers/gpu/drm/tiny/st7735r-sunxi-dbi.ko"
    "${PROJECT}/linux-drivers/nrf24/nrf24.ko"
)

IMG_FILE="t113-debian.img"
BOOT_SIZE_MB=32
ROOTFS_MIN_EXTRA_MB=800

# 计算 rootfs 大小
ROOTFS_USED_KB=$(du -s "${DEBIAN_ROOTFS_FILES}" 2>/dev/null | awk '{print $1}' || echo 0)
ROOTFS_SIZE_MB=$((ROOTFS_USED_KB / 1024 + ROOTFS_MIN_EXTRA_MB))
IMG_SIZE_MB=$((BOOT_SIZE_MB + ROOTFS_SIZE_MB))

echo "Creating image: ${IMG_FILE} (size: ${IMG_SIZE_MB} MB)"

# 1. 创建稀疏文件
truncate -s ${IMG_SIZE_MB}M "${IMG_FILE}"

# 2. 创建分区表
sudo fdisk "${IMG_FILE}" << EOF
o
n
p
1
2048
+${BOOT_SIZE_MB}M
n
p
2


t
1
c
a
1
w
EOF

# 3. 关联 loop 设备并自动创建分区
LOOP_DEV=$(sudo losetup -f --show -P "${IMG_FILE}")
BOOT_LOOP="${LOOP_DEV}p1"
ROOTFS_LOOP="${LOOP_DEV}p2"

# 4. 等待分区设备出现
for i in {1..5}; do
    [ -b "${BOOT_LOOP}" ] && [ -b "${ROOTFS_LOOP}" ] && break
    sleep 1
done

if [ ! -b "${BOOT_LOOP}" ] || [ ! -b "${ROOTFS_LOOP}" ]; then
    echo "Error: Partition devices not found" >&2
    sudo losetup -d "${LOOP_DEV}" 2>/dev/null || true
    exit 1
fi

# 5. 格式化分区
sudo mkfs.fat "${BOOT_LOOP}"
sudo mkfs.ext4 "${ROOTFS_LOOP}"

# 6. 挂载分区
mkdir -p "${MNT_DIR}/boot" "${MNT_DIR}/rootfs"
sudo mount "${BOOT_LOOP}" "${MNT_DIR}/boot"
sudo mount "${ROOTFS_LOOP}" "${MNT_DIR}/rootfs"

# 7. 写入 awboot
sudo dd if="${AWBOOT_FILE}" of="${IMG_FILE}" bs=1k seek=8 conv=notrunc

# 8. 复制文件
sudo cp "${KERNEL_FILE}" "${DTB_FILE}" "${MNT_DIR}/boot/"
sudo cp -rf "${DEBIAN_ROOTFS_FILES}/"* "${MNT_DIR}/rootfs/"

# ===== 内核模块安装：完整方案 =====
echo "Installing kernel modules..."

# 8.1 创建模块目录
sudo mkdir -p "${MNT_DIR}/rootfs/${MODULES_TARGET_DIR}"

# 8.2 【推荐】使用 make modules_install 安装完整模块树（包含 depmod 元数据）
echo "Installing full module tree via make modules_install..."
sudo make -C "${LINUX_SRC}" \
         ARCH=arm \
         CROSS_COMPILE=arm-linux-gnueabihf- \
         INSTALL_MOD_PATH="${MNT_DIR}/rootfs" \
         modules_install

# 8.3 【补充】复制额外的自定义模块（如 nrf24）
echo "Copying additional custom modules..."
for mod in "${MODULES_FILES[@]}"; do
    if [ -f "${mod}" ]; then
        # 获取模块所在目录结构（保持相对路径）
        MOD_SUBDIR=$(dirname "${mod#$LINUX_SRC/}")
        TARGET_SUBDIR="${MNT_DIR}/rootfs/${MODULES_TARGET_DIR}/${MOD_SUBDIR}"
        sudo mkdir -p "${TARGET_SUBDIR}"
        sudo cp "${mod}" "${TARGET_SUBDIR}/"
        echo "  Copied: $(basename "${mod}") -> ${TARGET_SUBDIR}"
    else
        echo "  Warning: Module not found: ${mod}"
    fi
done

# 8.4 【关键】重新运行 depmod 生成依赖关系（确保包含所有模块）
echo "Generating module dependencies with depmod..."
sudo chroot "${MNT_DIR}/rootfs" depmod -a "${KERNEL_VERSION}"

# 8.5 验证模块安装结果
echo "Verifying module installation..."
if [ -f "${MNT_DIR}/rootfs/${MODULES_TARGET_DIR}/modules.dep" ]; then
    echo "  ✓ modules.dep generated successfully"
else
    echo "  ⚠ Warning: modules.dep not found in ${MODULES_TARGET_DIR}"
fi

# 10. 清理挂载
sync
sudo umount "${MNT_DIR}/boot" "${MNT_DIR}/rootfs" 2>/dev/null || true
rmdir "${MNT_DIR}/boot" "${MNT_DIR}/rootfs" 2>/dev/null || true

# ===== 保留镜像优化但简化 =====
echo "Optimizing image..."

# 1. 先移除 ext4 的预留块（关键步骤！）
sudo tune2fs -m 0 "${ROOTFS_LOOP}"

# 2. 检查并修复文件系统
sudo e2fsck -f "${ROOTFS_LOOP}" -y

# 3. 收缩到最小尺寸
sudo resize2fs -M "${ROOTFS_LOOP}"

# 4. 重新读取分区信息
sudo partx -u "${LOOP_DEV}"

# 5. 获取 rootfs 分区的新大小（使用 blockdev 更可靠）
ROOTFS_SIZE_SECTORS=$(sudo blockdev --getsize "${ROOTFS_LOOP}")
BOOT_SIZE_SECTORS=$(sudo blockdev --getsize "${BOOT_LOOP}")

# 6. 计算新的总镜像大小（boot + rootfs + 保留一些尾部空间）
TOTAL_SECTORS=$((BOOT_SIZE_SECTORS + ROOTFS_SIZE_SECTORS + 2048))  # +1MB 保护空间
NEW_IMG_SIZE_BYTES=$((TOTAL_SECTORS * 512))

echo "New image size: $((NEW_IMG_SIZE_BYTES / 1024 / 1024)) MB"

# 7. 截断镜像文件
truncate -s "${NEW_IMG_SIZE_BYTES}" "${IMG_FILE}"

# 8. 清理
sudo losetup -d "${LOOP_DEV}"

IMG_SIZE=$(du -h "${IMG_FILE}" | cut -f1)
echo "✅ Image built successfully: ${IMG_FILE} (${IMG_SIZE})"
echo "   Kernel modules installed in: ${MODULES_TARGET_DIR}"
echo "   Use 'make flash-full' or 'make quick-flash' to flash and expand partitions automatically"