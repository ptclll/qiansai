#!/bin/bash
#
# 部署脚本 - 将 Flask 服务器部署到 Ubuntu 服务器
# 目标: ubuntu@175.178.79.44:/www/esp32-monitor/
#
# 使用方法:
#   在本地运行:  bash deploy.sh
#   或在服务器上直接: bash deploy.sh --local
#

set -e

SERVER_IP="175.178.79.44"
SERVER_USER="ubuntu"
SERVER_PORT="20"              # SSH 端口
REMOTE_DIR="/www/esp32-monitor"
APP_NAME="esp32-monitor"

echo "========================================="
echo "  🚀 部署 ESP32 监测系统到服务器"
echo "  目标: ${SERVER_USER}@${SERVER_IP}:${REMOTE_DIR}"
echo "========================================="

# ── 上传文件 ──
if [[ "$1" != "--local" ]]; then
    echo ""
    echo "[1/4] 📤 上传文件到服务器..."
    ssh -p ${SERVER_PORT} ${SERVER_USER}@${SERVER_IP} "mkdir -p ${REMOTE_DIR}/templates ${REMOTE_DIR}/static/images"
    
    # 只上传服务端需要的文件（image_sender.py 在本地 Windows 运行，不上传）
    scp -P ${SERVER_PORT} app.py ${SERVER_USER}@${SERVER_IP}:${REMOTE_DIR}/
    scp -P ${SERVER_PORT} requirements.txt ${SERVER_USER}@${SERVER_IP}:${REMOTE_DIR}/
    scp -P ${SERVER_PORT} templates/index.html ${SERVER_USER}@${SERVER_IP}:${REMOTE_DIR}/templates/
    
    echo "✅ 文件上传完成"
fi

# ── 安装依赖 ──
echo ""
echo "[2/4] 📦 安装 Python 依赖..."
ssh -p ${SERVER_PORT} ${SERVER_USER}@${SERVER_IP} "cd ${REMOTE_DIR} && pip3 install -r requirements.txt"

# ── 停止旧进程 ──
echo ""
echo "[3/4] 🛑 停止旧进程..."
ssh -p ${SERVER_PORT} ${SERVER_USER}@${SERVER_IP} "pkill -f 'python3.*app.py' || true"
sleep 1

# ── 启动 ──
echo ""
echo "[4/4] 🟢 使用 nohup 启动..."
ssh -p ${SERVER_PORT} ${SERVER_USER}@${SERVER_IP} "cd ${REMOTE_DIR} && nohup python3 app.py > /tmp/${APP_NAME}.log 2>&1 &"

sleep 2

# ── 验证 ──
echo ""
echo "🔍 验证服务状态..."
ssh -p ${SERVER_PORT} ${SERVER_USER}@${SERVER_IP} "ps aux | grep app.py | grep -v grep || echo '⚠ 进程未找到，检查日志: tail -f /tmp/${APP_NAME}.log'"

echo ""
echo "========================================="
echo "  ✅ 部署完成!"
echo ""
echo "  🌐 前端页面:   http://${SERVER_IP}:5000"
echo "  📤 上传图片:   http://${SERVER_IP}:5000/upload"
echo "  📊 数据 API:   http://${SERVER_IP}:5000/api/data"
echo "  📡 UDP 监听:   ${SERVER_IP}:5005"
echo ""
echo "  📋 查看日志: ssh -p ${SERVER_PORT} ${SERVER_USER}@${SERVER_IP} 'tail -f /tmp/${APP_NAME}.log'"
echo "  🛑 停止服务: ssh -p ${SERVER_PORT} ${SERVER_USER}@${SERVER_IP} 'pkill -f python3.*app.py'"
echo ""
echo "  💻 本地运行图片轮播: python image_sender.py --watch"
echo "========================================="
