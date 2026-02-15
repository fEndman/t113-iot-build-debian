# build_server.py
import PyInstaller.__main__
import os

def build():
    # PyInstaller 参数
    args = [
        'main.py',  # 你的主文件名
        '--onefile',  # 打包成单个exe文件
        '--windowed',  # 无控制台窗口
        '--icon=icon.ico',  # 图标文件（可选）
        '--name=DisplayStreamServer',  # 输出文件名
    ]
    # args = [
    #     'main.py',  # 你的主文件名
    #     '--onefile',  # 打包成单个exe文件
    #     '--console',  # 包含控制台
    #     '--icon=icon.ico',  # 图标文件（可选）
    #     '--name=DisplayStreamServerD',  # 输出文件名
    # ]
    
    PyInstaller.__main__.run(args)

if __name__ == '__main__':
    build()