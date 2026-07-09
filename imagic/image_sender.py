#!/usr/bin/env python3
"""
图片上传 & 轮播脚本（本地运行，上传到远程 Flask 服务器）

功能:
  1. 从本地文件夹读取图片，自动裁剪至 16:9
  2. 上传到远程 Flask 服务器
  3. 每秒发送一张图片，多张则轮流发送

用法:
  python image_sender.py                        # 交互模式
  python image_sender.py --watch                # 轮播模式：每秒从本地文件夹发送一张图片到服务器
  python image_sender.py --upload image.jpg     # 上传单张图片（裁剪 16:9 后发送到服务器）
  python image_sender.py --folder ./pics        # 批量上传文件夹中所有图片
  python image_sender.py --server 192.168.1.1:5000  # 指定服务器地址
"""

import os
import sys
import time
import argparse
import requests
from pathlib import Path
from PIL import Image

# ─── 默认配置 ───
SERVER_HOST = "175.178.79.44"
SERVER_PORT = 5000
SERVER_URL = f"http://{SERVER_HOST}:{SERVER_PORT}"

# 本地图片文件夹（脚本所在目录下的 send_images）
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LOCAL_IMAGES_DIR = os.path.join(SCRIPT_DIR, "send_images")

ALLOWED_EXTENSIONS = {'.png', '.jpg', '.jpeg', '.gif', '.webp', '.bmp'}

os.makedirs(LOCAL_IMAGES_DIR, exist_ok=True)


# ─── 裁剪为 16:9 ───
def crop_to_16_9(image_path, output_path=None):
    """将图片居中裁剪为 16:9"""
    img = Image.open(image_path)
    w, h = img.size
    target_ratio = 16.0 / 9.0
    current_ratio = w / h

    if abs(current_ratio - target_ratio) < 0.01:
        if output_path and output_path != image_path:
            img.save(output_path, quality=95)
        else:
            img.save(image_path, quality=95)
        print(f"    ✅ 已是 16:9 ({w}x{h})")
        return output_path or image_path

    if current_ratio > target_ratio:
        new_w = int(h * target_ratio)
        left = (w - new_w) // 2
        cropped = img.crop((left, 0, left + new_w, h))
    else:
        new_h = int(w / target_ratio)
        top = (h - new_h) // 2
        cropped = img.crop((0, top, w, top + new_h))

    save_path = output_path or image_path
    cropped.save(save_path, quality=95)
    print(f"    ✂️  裁剪 {w}x{h} -> {cropped.size[0]}x{cropped.size[1]} (16:9)")
    return save_path


# ─── 上传到远程服务器 ───
def upload_to_server(filepath):
    """上传单张图片到远程 Flask 服务器（服务器端也会自动裁剪）"""
    url = f"{SERVER_URL}/upload"
    filename = os.path.basename(filepath)

    try:
        with open(filepath, 'rb') as f:
            files = {'file': (filename, f, 'image/jpeg')}
            resp = requests.post(url, files=files, timeout=30)
            if resp.status_code == 200:
                return True
            else:
                print(f"    ❌ 服务器返回 {resp.status_code}: {resp.text[:100]}")
                return False
    except requests.ConnectionError:
        print(f"    ❌ 无法连接到服务器 {SERVER_URL}")
        return False
    except Exception as e:
        print(f"    ❌ 上传失败: {e}")
        return False


# ─── 获取本地图片列表 ───
def get_local_images():
    """返回本地 send_images 文件夹中的图片（按文件名排序）"""
    images = []
    if os.path.exists(LOCAL_IMAGES_DIR):
        for f in sorted(os.listdir(LOCAL_IMAGES_DIR)):
            ext = os.path.splitext(f)[1].lower()
            if ext in ALLOWED_EXTENSIONS:
                images.append(f)
    return images


# ─── 处理单张：裁剪 + 上传 ───
def process_and_upload(filepath):
    """裁剪为 16:9 并上传到服务器"""
    if not os.path.isfile(filepath):
        print(f"  ❌ 文件不存在: {filepath}")
        return False

    filename = os.path.basename(filepath)
    print(f"  📷 处理: {filename}")

    # 先裁剪到临时目录
    temp_dir = os.path.join(LOCAL_IMAGES_DIR, ".temp")
    os.makedirs(temp_dir, exist_ok=True)
    cropped_path = os.path.join(temp_dir, filename)

    crop_to_16_9(filepath, cropped_path)

    # 上传到服务器
    print(f"  📤 上传到 {SERVER_URL} ...")
    success = upload_to_server(cropped_path)

    if success:
        print(f"  ✅ 完成: {filename}")
    return success


# ─── 轮播模式：每秒发送一张图片 ───
def watch_and_send():
    """
    每秒从本地文件夹取一张图片发送到服务器。
    多张图片时轮流发送。
    发送前先裁剪为 16:9。
    """
    print(f"\n{'='*50}")
    print(f"  🔄 图片轮播模式")
    print(f"  📁 本地目录: {LOCAL_IMAGES_DIR}")
    print(f"  🌐 目标服务器: {SERVER_URL}")
    print(f"  按 Ctrl+C 停止")
    print(f"{'='*50}\n")

    idx = 0
    round_count = 0

    while True:
        images = get_local_images()

        if not images:
            print(f"\r  ⏳ 等待图片... 请将图片放入: {LOCAL_IMAGES_DIR}  ({time.strftime('%H:%M:%S')})", end='', flush=True)
            time.sleep(1)
            continue

        if idx % len(images) == 0 and idx > 0:
            round_count += 1
            print(f"\n  ── 第 {round_count} 轮 ──")

        current_idx = idx % len(images)
        current_img = images[current_idx]
        filepath = os.path.join(LOCAL_IMAGES_DIR, current_img)

        timestamp = time.strftime('%H:%M:%S')
        print(f"\r  📷 [{timestamp}] 第{current_idx+1}/{len(images)}张: {current_img}", end='', flush=True)

        # 裁剪 + 上传
        process_and_upload(filepath)

        idx += 1
        time.sleep(1)


# ─── 交互模式 ───
def interactive_mode():
    print("=" * 50)
    print("  🖼️  图片上传 & 轮播工具（本地 → 远程服务器）")
    print("=" * 50)
    print(f"  🌐 服务器: {SERVER_URL}")
    print(f"  📁 本地图片目录: {LOCAL_IMAGES_DIR}")
    print()

    while True:
        print("\n选项:")
        print("  1. 上传单张图片（自动裁剪 16:9）")
        print("  2. 批量上传文件夹中所有图片")
        print("  3. 🚀 开始轮播（每秒发送一张，轮流）")
        print("  4. 查看本地图片列表")
        print("  5. 清空本地图片")
        print("  0. 退出")
        choice = input("\n请选择: ").strip()

        if choice == '1':
            filepath = input("图片路径（可拖入）: ").strip().strip('"').strip("'")
            if os.path.isfile(filepath):
                process_and_upload(filepath)
            else:
                print("  ❌ 文件不存在")

        elif choice == '2':
            folder = input("文件夹路径: ").strip().strip('"').strip("'")
            if os.path.isdir(folder):
                files = [f for f in sorted(os.listdir(folder))
                         if os.path.splitext(f)[1].lower() in ALLOWED_EXTENSIONS]
                if not files:
                    print("  ⚠ 文件夹中没有支持的图片格式")
                else:
                    print(f"  找到 {len(files)} 张图片，开始处理...")
                    success = 0
                    for f in files:
                        if process_and_upload(os.path.join(folder, f)):
                            success += 1
                    print(f"\n  ✅ 完成: {success}/{len(files)} 张上传成功")
            else:
                print("  ❌ 文件夹不存在")

        elif choice == '3':
            watch_and_send()

        elif choice == '4':
            images = get_local_images()
            print(f"\n  本地图片 ({len(images)} 张):")
            if not images:
                print(f"    (空)  请放入图片到: {LOCAL_IMAGES_DIR}")
            for i, img in enumerate(images, 1):
                filepath = os.path.join(LOCAL_IMAGES_DIR, img)
                try:
                    im = Image.open(filepath)
                    print(f"    {i}. {img}  ({im.size[0]}x{im.size[1]})")
                except:
                    print(f"    {i}. {img}")

        elif choice == '5':
            confirm = input("确认清空本地图片? (yes/no): ").strip()
            if confirm.lower() == 'yes':
                for img in get_local_images():
                    os.remove(os.path.join(LOCAL_IMAGES_DIR, img))
                print("  ✅ 已清空")

        elif choice == '0':
            print("👋 再见!")
            break


# ─── 入口 ───
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='图片上传 & 轮播工具（本地 → 远程 Flask 服务器）')
    parser.add_argument('--server', type=str, default=None,
                        help=f'服务器地址 (默认: {SERVER_URL})')
    parser.add_argument('--watch', action='store_true',
                        help='轮播模式：每秒从本地文件夹发送一张图片到服务器')
    parser.add_argument('--upload', type=str,
                        help='上传单张图片（自动裁剪 16:9 后发送）')
    parser.add_argument('--folder', type=str,
                        help='批量上传文件夹中所有图片')
    parser.add_argument('--list', action='store_true',
                        help='列出本地图片')

    args = parser.parse_args()

    if args.server:
        if '://' not in args.server:
            SERVER_URL = f"http://{args.server}"
        else:
            SERVER_URL = args.server
    else:
        SERVER_URL = f"http://{SERVER_HOST}:{SERVER_PORT}"

    if args.upload:
        if os.path.isfile(args.upload):
            process_and_upload(args.upload)
        else:
            print(f"❌ 文件不存在: {args.upload}")
    elif args.folder:
        if os.path.isdir(args.folder):
            files = [f for f in sorted(os.listdir(args.folder))
                     if os.path.splitext(f)[1].lower() in ALLOWED_EXTENSIONS]
            print(f"找到 {len(files)} 张图片，开始处理...")
            success = 0
            for f in files:
                if process_and_upload(os.path.join(args.folder, f)):
                    success += 1
            print(f"✅ 完成: {success}/{len(files)} 张上传成功")
        else:
            print(f"❌ 文件夹不存在: {args.folder}")
    elif args.watch:
        watch_and_send()
    elif args.list:
        images = get_local_images()
        print(f"本地图片 ({len(images)} 张):")
        for i, img in enumerate(images, 1):
            print(f"  {i}. {img}")
    else:
        interactive_mode()
