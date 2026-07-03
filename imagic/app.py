#!/usr/bin/env python3
"""
Flask 服务器 - 接收 ESP32 UDP 数据 + Web 前端展示
运行: python app.py
"""

import json
import os
import socket
import threading
import time
from flask import Flask, render_template, jsonify, request, send_from_directory
from werkzeug.utils import secure_filename
from PIL import Image
from pathlib import Path

app = Flask(__name__)

# ─── 配置 ───
UDP_PORT = 5005
UPLOAD_FOLDER = os.path.join(os.path.dirname(__file__), 'static', 'images')
ALLOWED_EXTENSIONS = {'png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp'}

os.makedirs(UPLOAD_FOLDER, exist_ok=True)
app.config['UPLOAD_FOLDER'] = UPLOAD_FOLDER
app.config['MAX_CONTENT_LENGTH'] = 16 * 1024 * 1024  # 16MB

# ─── 共享状态 ───
sensor_data = {
    "Humidity": None,
    "Temperature": None,
    "co2": None,
    "pressure": None,
    "timestamp": None
}
data_lock = threading.Lock()

# 记录最近一次发送数据的 ESP32 地址 (ip, port)
last_esp32_addr = None
addr_lock = threading.Lock()

# ─── 全局 UDP socket（收发共用同一端口，穿透对称 NAT） ───
udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
udp_sock.bind(('0.0.0.0', UDP_PORT))
udp_sock.settimeout(1.0)
udp_sock_lock = threading.Lock()  # 保护 sendto 操作

# ─── UDP 监听线程 ───
def udp_listener():
    """监听 ESP32 发来的 UDP 数据包（复用全局 socket）"""
    print(f"[UDP] 监听端口 {UDP_PORT} ...")

    while True:
        try:
            data, addr = udp_sock.recvfrom(4096)
        except socket.timeout:
            continue

        msg = data.decode('utf-8', errors='replace').strip()
        print(f"[UDP] 收到 {addr}: {msg}")

        # 记录最近发送数据的 ESP32 地址
        with addr_lock:
            global last_esp32_addr
            last_esp32_addr = addr

        if msg.lower() == "null":
            with data_lock:
                sensor_data["timestamp"] = time.time()
            continue

        try:
            parsed = json.loads(msg)
            with data_lock:
                if "Humidity" in parsed:
                    sensor_data["Humidity"] = parsed["Humidity"]
                if "Temperature" in parsed:
                    sensor_data["Temperature"] = parsed["Temperature"]
                if "co2" in parsed:
                    sensor_data["co2"] = parsed["co2"]
                if "pressure" in parsed:
                    sensor_data["pressure"] = parsed["pressure"]
                sensor_data["timestamp"] = time.time()
            print(f"[UDP] 更新数据: {parsed}")
        except json.JSONDecodeError:
            print(f"[UDP] JSON 解析失败: {msg}")


# ─── 辅助函数 ───
def allowed_file(filename):
    return '.' in filename and filename.rsplit('.', 1)[1].lower() in ALLOWED_EXTENSIONS


def crop_to_16_9(image_path, output_path=None):
    """将图片裁剪为 16:9"""
    img = Image.open(image_path)
    w, h = img.size
    target_ratio = 16.0 / 9.0
    current_ratio = w / h

    if current_ratio > target_ratio:
        # 图片太宽，裁左右
        new_w = int(h * target_ratio)
        left = (w - new_w) // 2
        cropped = img.crop((left, 0, left + new_w, h))
    else:
        # 图片太高，裁上下
        new_h = int(w / target_ratio)
        top = (h - new_h) // 2
        cropped = img.crop((0, top, w, top + new_h))

    save_path = output_path or image_path
    cropped.save(save_path, quality=95)
    print(f"[裁剪] {image_path} -> {cropped.size}")
    return save_path


def get_image_list():
    """获取 images 文件夹中所有图片，按文件名排序"""
    images = []
    if os.path.exists(UPLOAD_FOLDER):
        for f in sorted(os.listdir(UPLOAD_FOLDER)):
            if allowed_file(f):
                images.append(f)
    return images


# ─── 路由 ───

@app.route('/')
def index():
    """主页"""
    return render_template('index.html')


@app.route('/api/data')
def api_data():
    """返回最新传感器数据"""
    with data_lock:
        return jsonify({
            "Humidity": sensor_data["Humidity"],
            "Temperature": sensor_data["Temperature"],
            "co2": sensor_data["co2"],
            "pressure": sensor_data["pressure"],
            "timestamp": sensor_data["timestamp"]
        })


@app.route('/api/images')
def api_images():
    """返回当前图片列表"""
    images = get_image_list()
    return jsonify({"images": images, "count": len(images)})


@app.route('/api/current_image')
def api_current_image():
    """返回当前应显示的图片（按时间轮转）"""
    images = get_image_list()
    if not images:
        return jsonify({"image": None})
    idx = int(time.time()) % len(images)
    return jsonify({"image": images[idx]})


@app.route('/images/<path:filename>')
def serve_image(filename):
    """提供图片文件"""
    return send_from_directory(UPLOAD_FOLDER, filename)


# ─── 控制页面路由 ───

@app.route('/control')
def control_page():
    """ESP32 方向控制页面"""
    return render_template('control.html')


@app.route('/api/control', methods=['POST'])
def api_control():
    """向 ESP32 发送方向控制指令"""
    data = request.get_json()
    if not data or 'cmd' not in data:
        return jsonify({"ok": False, "msg": "缺少 cmd 参数"}), 400

    cmd = data['cmd']
    if cmd not in ('up', 'down', 'left', 'right', 'stop'):
        return jsonify({"ok": False, "msg": f"无效指令: {cmd}"}), 400

    with addr_lock:
        addr = last_esp32_addr

    if addr is None:
        return jsonify({"ok": False, "msg": "ESP32 尚未上线，无目标地址"}), 503

    # 通过共享 UDP socket 发送控制指令（源端口=5005，穿透 NAT）
    try:
        payload = json.dumps({"cmd": cmd})
        with udp_sock_lock:
            udp_sock.sendto(payload.encode('utf-8'), addr)
        print(f"[CTRL] 发送 '{cmd}' -> {addr[0]}:{addr[1]}")
        return jsonify({"ok": True, "cmd": cmd, "target": f"{addr[0]}:{addr[1]}"})
    except Exception as e:
        print(f"[CTRL] 发送失败: {e}")
        return jsonify({"ok": False, "msg": str(e)}), 500


@app.route('/upload', methods=['GET', 'POST'])
def upload():
    """上传图片页面 + 处理"""
    if request.method == 'POST':
        if 'file' not in request.files:
            return "请选择文件", 400
        file = request.files['file']
        if file.filename == '':
            return "未选择文件", 400
        if file and allowed_file(file.filename):
            filename = secure_filename(file.filename)
            filepath = os.path.join(UPLOAD_FOLDER, filename)
            file.save(filepath)
            # 自动裁剪为 16:9
            crop_to_16_9(filepath)
            return f'<h2 style="color:green;font-family:sans-serif">✅ 上传成功! {filename}</h2><a href="/upload">继续上传</a> | <a href="/">返回首页</a>'
        return "不支持的文件格式", 400

    return '''
    <!doctype html>
    <html><head><meta charset="utf-8"><title>上传图片</title>
    <style>
    :root{--bg:#0f172a;--txt:#e2e8f0;--accent:#22d3ee}
    body{background:var(--bg);color:var(--txt);font-family:sans-serif;display:flex;
    align-items:center;justify-content:center;min-height:100vh;margin:0}
    .box{background:#111827;padding:40px;border-radius:16px;text-align:center}
    input[type=file]{margin:16px 0;color:var(--txt)}
    button{background:var(--accent);border:none;padding:12px 32px;border-radius:8px;
    font-size:16px;cursor:pointer;color:#000;font-weight:bold}
    a{color:var(--accent)}
    </style></head><body><div class="box">
    <h1>📤 上传图片</h1>
    <p>自动裁剪为 16:9</p>
    <form method="post" enctype="multipart/form-data">
    <input type="file" name="file" accept="image/*"><br>
    <button type="submit">上传</button>
    </form>
    <p style="margin-top:20px"><a href="/">← 返回首页</a></p>
    </div></body></html>
    '''


# ─── 启动 ───
if __name__ == '__main__':
    # 启动 UDP 监听线程
    threading.Thread(target=udp_listener, daemon=True).start()

    print("\n" + "="*50)
    print("  🌐 Flask 服务器启动")
    print(f"  📡 UDP 监听: 0.0.0.0:{UDP_PORT}")
    print(f"  🖼️  图片目录: {UPLOAD_FOLDER}")
    print("="*50 + "\n")

    app.run(host='0.0.0.0', port=5000, debug=False)
