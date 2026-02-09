import os

def collect_py_files():
    """收集所有py文件并打包成txt"""
    output_file = "source_code.txt"
    
    with open(output_file, 'w', encoding='utf-8') as outfile:
        for root, dirs, files in os.walk('.'):
            # 跳过隐藏目录和__pycache__
            dirs[:] = [d for d in dirs if not d.startswith('.') and d != '__pycache__']
            
            for file in files:
                if file.endswith('.py'):
                    filepath = os.path.join(root, file)
                    
                    # 写入分隔线和文件路径
                    outfile.write('=' * 60 + '\n')
                    outfile.write(filepath.lstrip('./') + '\n')
                    outfile.write('=' * 60 + '\n')
                    
                    # 读取并写入文件内容
                    try:
                        with open(filepath, 'r', encoding='utf-8') as infile:
                            content = infile.read()
                            outfile.write(content)
                            if not content.endswith('\n'):
                                outfile.write('\n')
                    except UnicodeDecodeError:
                        print(f"跳过二进制文件: {filepath}")
                        outfile.write(f"# [BINARY FILE SKIPPED: {filepath}]\n")
                    except Exception as e:
                        print(f"读取文件失败: {filepath}, 错误: {e}")
                        outfile.write(f"# [ERROR READING FILE: {filepath}]\n")
                    
                    outfile.write('\n')  # 文件间空行
    
    print(f"已将所有py文件打包到: {output_file}")

if __name__ == "__main__":
    collect_py_files()