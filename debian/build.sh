#!/bin/bash
# build.sh
# 用法: ./build.sh [输出目录] [可选: 镜像源]

set -euo pipefail
trap 'echo "❌ 脚本在第${LINENO}行出错，退出码: $?" >&2' ERR

# ========== 配置参数 ==========
DEBIAN_VERSION="bookworm"
ARCH="armhf"
MIRROR="${2:-http://mirrors.tuna.tsinghua.edu.cn/debian/}"  # 默认清华镜像
OUTPUT_DIR="${1:-./rootfs}"
MIN_DISK_SPACE_MB=800  # 最小所需磁盘空间 (MB)

# ========== 辅助函数 ==========
log() { echo -e "\033[1;34m[INFO]\033[0m $1"; }
success() { echo -e "\033[1;32m✓ $1\033[0m"; }
error() { echo -e "\033[1;31m✗ $1\033[0m" >&2; exit 1; }
check_space() {
    local avail=$(df -m "${OUTPUT_DIR%/*}" | awk 'NR==2 {print $4}')
    [[ $avail -ge $MIN_DISK_SPACE_MB ]] || error "可用空间不足 ($avail MB)，至少需要 $MIN_DISK_SPACE_MB MB"
}

# ========== 环境检查 ==========
[[ $EUID -eq 0 ]] && error "❌ 请勿以 root 身份直接运行，脚本内部会调用 sudo"
command -v debootstrap >/dev/null || error "未安装 debootstrap (sudo apt install debootstrap)"
command -v qemu-arm-static >/dev/null || error "未安装 qemu-user-static (sudo apt install qemu-user-static)"

log "检查磁盘空间..."
check_space

# ========== 创建输出目录 ==========
if [[ -d "$OUTPUT_DIR" ]]; then
    read -rp "⚠️ 目录 $OUTPUT_DIR 已存在，是否覆盖? (y/N): " -n 1 -r
    echo
    [[ ! $REPLY =~ ^[Yy]$ ]] && error "用户取消操作"
    sudo rm -rf "$OUTPUT_DIR"
fi
mkdir -p "$OUTPUT_DIR"
log "输出目录: $OUTPUT_DIR"

# ========== 第一阶段: debootstrap ==========
log "阶段1: 下载基础系统 ($ARCH/$DEBIAN_VERSION)..."
sudo debootstrap --arch="$ARCH" --foreign --no-check-gpg "$DEBIAN_VERSION" "$OUTPUT_DIR" "$MIRROR" || error "debootstrap 第一阶段失败"

# ========== 第二阶段: qemu 模拟完成安装 ==========
log "阶段2: 通过 qemu 完成安装..."
sudo cp "$(command -v qemu-arm-static)" "$OUTPUT_DIR/usr/bin/" || error "复制 qemu-arm-static 失败"

# 挂载虚拟文件系统 (便于后续 chroot)
for fs in proc sys dev/pts; do
    sudo mkdir -p "$OUTPUT_DIR/$fs"
    case $fs in
        proc) sudo mount -t proc proc "$OUTPUT_DIR/proc" ;;
        sys)  sudo mount -t sysfs sys "$OUTPUT_DIR/sys" ;;
        dev/pts) sudo mount -t devpts devpts "$OUTPUT_DIR/dev/pts" ;;
    esac
done

# 执行第二阶段
sudo chroot "$OUTPUT_DIR" /debootstrap/debootstrap --second-stage || error "debootstrap 第二阶段失败"

# ========== 基础配置 ==========
log "配置系统基础环境..."

# 主机名
echo "t113-iot" | sudo tee "$OUTPUT_DIR/etc/hostname" >/dev/null

# sources.list (使用配置的镜像源)
cat <<EOF | sudo tee "$OUTPUT_DIR/etc/apt/sources.list" >/dev/null
deb $MIRROR $DEBIAN_VERSION main contrib non-free non-free-firmware
deb $MIRROR $DEBIAN_VERSION-updates main contrib non-free non-free-firmware
deb http://security.debian.org/debian-security $DEBIAN_VERSION-security main contrib non-free non-free-firmware
EOF

# 时区
echo "Asia/Shanghai" | sudo tee "$OUTPUT_DIR/etc/timezone" >/dev/null
sudo chroot "$OUTPUT_DIR" dpkg-reconfigure -f noninteractive tzdata >/dev/null 2>&1 || true

# 更新包列表
log "更新包索引..."
sudo chroot "$OUTPUT_DIR" apt update

# 定义要安装的包列表
PACKAGES=(
    locales net-tools iproute2 vim-tiny wget curl ca-certificates network-manager sudo zram-tools
    openssh-server systemd-sysv wireless-tools wpasupplicant udhcpd passwd systemd-timesyncd usbutils
    firmware-realtek firmware-brcm80211 firmware-atheros firmware-libertas 
    file kbd console-setup xfonts-terminus tmux git htop cpufrequtils dos2unix evtest iperf
    alsa-utils alsa-tools bluetooth bluez bluez-tools mpv v4l-utils ffmpeg
    libdrm-dev libffi-dev portaudio19-dev libglib2.0-dev

    python3-dev python3-pip python3-venv python3-psutil python3-evdev
    python3-pyqt6 python3-mpv python3-pyaudio python3-numpy python3-scipy python3-pydub
)

# 一次性安装所有包，保留关键输出
log "开始安装基础软件包 (${#PACKAGES[@]} 个)..."
PACKAGE_LIST=$(printf '%s ' "${PACKAGES[@]}")
if ! sudo chroot "$OUTPUT_DIR" env DEBIAN_FRONTEND=noninteractive apt install -y --no-install-recommends $PACKAGE_LIST; then
    error "基础包安装失败，请检查上述错误信息"
fi

# 设置zram swap
log "配置 zram 交换空间 (96MB, zstd 压缩)..."
sudo tee -a "$OUTPUT_DIR/etc/default/zramswap" > /dev/null <<'EOF'
ALGO=zstd
SIZE=96
PRIORITY=100
EOF

# 设置主频调度器策略
log "配置主频调度器策略..."
sudo tee -a "$OUTPUT_DIR/etc/default/cpufrequtils" > /dev/null <<'EOF'
GOVERNOR="schedutil"
EOF

# 生成 locale
echo "en_US.UTF-8 UTF-8" | sudo tee "$OUTPUT_DIR/etc/locale.gen" >/dev/null
sudo chroot "$OUTPUT_DIR" locale-gen >/dev/null 2>&1
echo "LANG=en_US.UTF-8" | sudo tee "$OUTPUT_DIR/etc/default/locale" >/dev/null

# 创建必要目录
sudo mkdir -p "$OUTPUT_DIR/{tmp,run,var/run}"

# 创建BT固件链接
sudo mkdir -p "$OUTPUT_DIR/lib/firmware/rtl_bt"
sudo ln -sf rtl8723bs_config-OBDA8723.bin "$OUTPUT_DIR/lib/firmware/rtl_bt/rtl8723bs_config.bin"

# ========== 设置默认 root 密码 ==========
log "设置 root 用户默认密码为 'root'（仅用于开发测试！）..."
# 使用 chpasswd 在 chroot 内设置明文密码（需已安装 passwd 包）
echo 'root:root' | sudo chroot "$OUTPUT_DIR" chpasswd || error "设置 root 密码失败"

# ========== 修复关键权限 ==========
log "修复sudo权限..."
sudo chown root:root "$OUTPUT_DIR/usr/bin/sudo" 2>/dev/null || true
sudo chmod 4755 "$OUTPUT_DIR/usr/bin/sudo" 2>/dev/null || true

# ========== 配置控制台字体 ==========
log "配置控制台字体..."
sudo tee "$OUTPUT_DIR/etc/default/console-setup" > /dev/null << 'EOF'
# CONFIGURATION FILE FOR SETUPCON
# Consult the console-setup(5) manual page.
ACTIVE_CONSOLES="/dev/tty[1-6]"
CHARMAP="UTF-8"
CODESET="Lat15"
FONTFACE="Terminus"
FONTSIZE="6x12"
VIDEOMODE=
# The following is an example how to use a braille font
# FONT='lat9w-08.psf.gz brl-8x8.psf'
EOF

# ========== 配置 RNDIS (192.168.113.0/24) ==========
log "配置 RNDIS 网络 (192.168.113.0/24)..."

# 1. 配置 udhcpd.conf（仅分配 IP）
sudo tee "$OUTPUT_DIR/etc/udhcpd.conf" > /dev/null << 'EOF'
start      192.168.113.2
end        192.168.113.10
interface  usb0
remaining  yes
opt        subnet 255.255.255.0
EOF

# 2. 安全启用 udhcpd：仅设置 DHCPD_ENABLED="yes"，保留原文件其余内容
sudo tee "$OUTPUT_DIR/etc/default/udhcpd" > /dev/null << 'EOF'
# Comment the following line to enable
DHCPD_ENABLED="yes"

# Options to pass to busybox' udhcpd.
#
# -S    Log to syslog
# -f    run in foreground

DHCPD_OPTS="-S"
EOF

# 3. 创建 setup-rndis.sh 脚本
sudo tee "$OUTPUT_DIR/usr/local/bin/setup-rndis.sh" > /dev/null << 'EOF'
#!/bin/sh
set -e

G=/sys/kernel/config/usb_gadget/g1
UDC=$(cat /sys/class/udc/*/name 2>/dev/null || ls /sys/class/udc | head -n1)

[ -f "$G/UDC" ] && echo "" > "$G/UDC"
rm -rf "$G"

mkdir -p "$G"
echo 0x1d6b > "$G/idVendor"
echo 0x0104 > "$G/idProduct"
mkdir -p "$G/strings/0x409"
echo "t113_rndis" > "$G/strings/0x409/serialnumber"
echo "T113 RNDIS" > "$G/strings/0x409/product"

mkdir -p "$G/configs/c.1"
echo 250 > "$G/configs/c.1/MaxPower"

mkdir -p "$G/functions/rndis.usb0"
echo "02:11:22:33:01:13" > "$G/functions/rndis.usb0/host_addr"
echo "02:11:22:34:01:13" > "$G/functions/rndis.usb0/dev_addr"
ln -s "$G/functions/rndis.usb0" "$G/configs/c.1/"

echo "$UDC" > "$G/UDC"

ip addr add 192.168.113.1/24 dev usb0 2>/dev/null || true
ip link set usb0 up
EOF

sudo chmod +x "$OUTPUT_DIR/usr/local/bin/setup-rndis.sh"

# 4. 创建 systemd 服务
sudo tee "$OUTPUT_DIR/etc/systemd/system/setup-rndis.service" > /dev/null << 'EOF'
[Unit]
Description=RNDIS Gadget Setup
DefaultDependencies=no
After=local-fs.target
Before=network-pre.target
ConditionPathExists=/sys/class/udc

[Service]
Type=oneshot
ExecStart=/usr/local/bin/setup-rndis.sh
RemainAfterExit=yes

[Install]
WantedBy=sysinit.target
EOF

# 5. 启用服务
sudo chroot "$OUTPUT_DIR" systemctl enable setup-rndis.service
sudo chroot "$OUTPUT_DIR" systemctl enable udhcpd.service

# ========== 清理 ==========
log "清理临时文件..."
sudo rm -f "$OUTPUT_DIR/usr/bin/qemu-arm-static"
sudo chroot "$OUTPUT_DIR" apt clean
sudo chroot "$OUTPUT_DIR" rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

# 卸载虚拟文件系统
for fs in dev/pts sys proc; do
    sudo umount -l "$OUTPUT_DIR/$fs" 2>/dev/null || true
done

# ========== 完成报告 ==========
success "✅ rootfs 构建成功!"
success "📦 已安装软件包详情:"
sudo chroot "$OUTPUT_DIR" dpkg -l | grep -E "^ii" | wc -l | xargs -I {} echo "   总计安装包数量: {}"