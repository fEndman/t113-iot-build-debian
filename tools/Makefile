PROJECT	:= ..

MNT_DIR	:=	./mnt
$(MNT_DIR):
	mkdir -p $(MNT_DIR)

AWBOOT_FILE			:= 	$(PROJECT)/awboot/awboot-boot-sd.bin
DTB_FILE 			:= 	$(PROJECT)/linux-6.1-t113/arch/arm/boot/dts/sun8i-t113-iot-station.dtb
KERNEL_FILE			:= 	$(PROJECT)/linux-6.1-t113/arch/arm/boot/zImage

DEBIAN_ROOTFS_FILES	:= 	$(PROJECT)/debian/rootfs

DEV_FILE ?= 
DEV_CORE_FILE	:=	$(DEV_FILE)1
DEV_ROOTFS_FILE	:=	$(DEV_FILE)2

check-dev-file:
ifndef DEV_FILE
	$(error Please specify DEV_FILE parameter, e.g., make flash-sd DEV_FILE=/dev/sdd)
endif

SFTP_USERNAME	:=	fjj
SFTP_HOST		:=	192.168.113.1


help:
	@echo "use: make download-awboot	# Download awboot to SD card"
	@echo "     make build-img     		# Build SD card image file"
	@echo "     make flash-sd      		# Flash image to SD card"
	@echo "     make clean-img     		# Remove image file"
	@echo "     make chroot-img     	# Chroot into image file"
	@echo "     make sftp-download-core	# Download kernel and dtb to deployed device SFTP"

download-awboot: check-dev-file
	sudo dd if=$(AWBOOT_FILE) of=$(DEV_FILE) bs=1k seek=8

sftp-download-core:
	./sftp-upload-core.sh $(SFTP_USERNAME) $(SFTP_HOST) $(KERNEL_FILE) $(DTB_FILE) /boot

chroot-sd: check-dev-file $(MNT_DIR)
	sudo mount $(DEV_ROOTFS_FILE) $(MNT_DIR)
	sudo mount --bind /etc/resolv.conf $(MNT_DIR)/etc/resolv.conf
	sudo mount -t proc /proc $(MNT_DIR)/proc
	sudo mount -t sysfs /sys $(MNT_DIR)/sys
	sudo mount -o bind /dev $(MNT_DIR)/dev
	sudo LC_ALL=C LANGUAGE=C LANG=C chroot $(MNT_DIR)
	sudo umount $(MNT_DIR)/etc/resolv.conf
	sudo umount $(MNT_DIR)/proc
	sudo umount $(MNT_DIR)/sys
	sudo umount $(MNT_DIR)/dev
	sync
	sudo umount $(MNT_DIR)

remake-sd: check-dev-file
	sudo ./remake-sd.sh $(DEV_FILE) $(DEV_CORE_FILE) $(DEV_ROOTFS_FILE)

# ===== 镜像构建 =====
.PHONY: build-img flash-sd clean-img chroot-img

# 镜像文件名
IMG_FILE := t113-debian.img

build-img:
	./build-img.sh

flash-sd: check-dev-file $(MNT_DIR)
	@test -f $(IMG_FILE) || { echo "Error: $(IMG_FILE) not found. Run 'make build-img' first."; exit 1; }
	@echo "Flashing $(IMG_FILE) to $(DEV_FILE)..."
	
    # 1. 烧录
	sudo dd if=$(IMG_FILE) of=$(DEV_FILE) bs=1M status=progress oflag=sync
	
    # 2. 扩展分区
	sudo parted $(DEV_FILE) --script resizepart 2 100%
	sudo partprobe $(DEV_FILE)
	sleep 2
	
    # 3. 扩展文件系统
	sudo e2fsck -f -p $(DEV_FILE)2 2>/dev/null || true
	sudo resize2fs $(DEV_FILE)2
	
	@echo "Creating swap file..."
    # 挂载根分区
	sudo mount $(DEV_ROOTFS_FILE) $(MNT_DIR)
	
    # 创建 200MB swap 文件
	sudo dd if=/dev/zero of=$(MNT_DIR)/swapfile bs=1M count=200
	sudo chmod 600 $(MNT_DIR)/swapfile
	sudo mkswap $(MNT_DIR)/swapfile
	
    # 配置 fstab
	sudo sh -c 'echo "# <file system> <mount point>   <type>  <options>       <dump>  <pass>" > $(MNT_DIR)/etc/fstab'
	sudo sh -c 'echo "/dev/mmcblk0p2  /               ext4    noatime,nodiratime,commit=120,errors=remount-ro        0       1" >> $(MNT_DIR)/etc/fstab'
	sudo sh -c 'echo "/dev/mmcblk0p1  /boot           vfat    defaults        0       2" >> $(MNT_DIR)/etc/fstab'
	sudo sh -c 'echo "/swapfile       none            swap    sw              0       0" >> $(MNT_DIR)/etc/fstab'
	
    # 卸载分区
	sudo umount $(MNT_DIR)
	
	sudo sync

	@echo "✅ Done!"

clean-img:
	rm -f $(IMG_FILE)

chroot-img:
	./chroot-img.sh