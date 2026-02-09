#!/bin/bash

if [ $# -lt 4 ] || [ $# -gt 5 ]; then
    echo "Usage: $0 <username> <host> <kernel_file> <dtb_file> [target_dir]"
    exit 1
fi

USERNAME=$1
HOST=$2
KERNEL_FILE=$3
DTB_FILE=$4
TARGET_DIR=${5:-"/boot"}

echo "Uploading kernel and single DTB file to $HOST:$TARGET_DIR..."

# 验证本地文件存在
if [ ! -f "$KERNEL_FILE" ]; then
    echo "Error: Kernel file not found: $KERNEL_FILE"
    exit 1
fi

if [ ! -f "$DTB_FILE" ]; then
    echo "Error: DTB file not found: $DTB_FILE"
    exit 1
fi

# 创建临时目录
TEMP_DIR=$(mktemp -d)
cp "$KERNEL_FILE" "$TEMP_DIR/zImage"

# 提取DTB文件名（不含路径）并直接放在boot目录下
DTB_NAME=$(basename "$DTB_FILE")
cp "$DTB_FILE" "$TEMP_DIR/$DTB_NAME"

# SSH连接参数
SSH_OPTS="-o StrictHostKeyChecking=no -o ControlMaster=auto -o ControlPath=/tmp/ssh-%r@%h:%p -o ControlPersist=60"

# 建立连接复用（第一次连接会要求输入密码）
echo "Establishing SSH connection..."
ssh $SSH_OPTS "$USERNAME@$HOST" "echo 'Connected successfully'" || {
    echo "Failed to connect to remote host"
    rm -rf "$TEMP_DIR"
    exit 1
}

# 上传到用户的临时目录
UPLOAD_DIR="/tmp/deploy_$$"  # 使用进程ID确保唯一性
echo "Creating temporary directory on remote host..."

# 创建临时目录（使用已建立的连接）
if ! ssh $SSH_OPTS "$USERNAME@$HOST" "mkdir -p $UPLOAD_DIR"; then
    echo "Failed to create remote directory"
    rm -rf "$TEMP_DIR"
    exit 1
fi

# 上传zImage（使用已建立的连接）
echo "Uploading zImage..."
if scp $SSH_OPTS "$TEMP_DIR/zImage" "$USERNAME@$HOST:$UPLOAD_DIR/"; then
    echo "zImage uploaded successfully"
else
    echo "Failed to upload zImage"
    ssh $SSH_OPTS "$USERNAME@$HOST" "rm -rf $UPLOAD_DIR"
    rm -rf "$TEMP_DIR"
    exit 1
fi

# 上传DTB文件（使用已建立的连接）
echo "Uploading DTB file..."
if scp $SSH_OPTS "$TEMP_DIR/$DTB_NAME" "$USERNAME@$HOST:$UPLOAD_DIR/"; then
    echo "DTB file uploaded successfully"
else
    echo "Failed to upload DTB file"
    ssh $SSH_OPTS "$USERNAME@$HOST" "rm -rf $UPLOAD_DIR"
    rm -rf "$TEMP_DIR"
    exit 1
fi

# 使用sudo移动到目标目录（需要sudo密码）
echo "Moving files to $TARGET_DIR with sudo..."
echo "Please enter sudo password when prompted:"
if ssh -t $SSH_OPTS "$USERNAME@$HOST" "sudo cp $UPLOAD_DIR/zImage $TARGET_DIR/ && sudo cp $UPLOAD_DIR/$DTB_NAME $TARGET_DIR/ && sudo chmod 644 $TARGET_DIR/zImage $TARGET_DIR/$DTB_NAME && sudo sync && echo 'Files copied successfully' && sudo rm -rf $UPLOAD_DIR"; then
    echo "Files moved to $TARGET_DIR successfully"
else
    echo "Failed to move files to $TARGET_DIR"
    ssh $SSH_OPTS "$USERNAME@$HOST" "rm -rf $UPLOAD_DIR"
    rm -rf "$TEMP_DIR"
    exit 1
fi

# 关闭连接复用
ssh -O exit $SSH_OPTS "$USERNAME@$HOST" 2>/dev/null || true

# 清理本地临时文件
rm -rf "$TEMP_DIR"

echo "Update completed successfully!"
echo "Uploaded:"
echo "  - zImage to $TARGET_DIR/"
echo "  - $DTB_NAME to $TARGET_DIR/"