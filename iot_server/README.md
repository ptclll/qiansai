# ESP32-S3 WebSocket Serial Bridge

## 架构

```
┌─────────────┐    WebSocket     ┌──────────────┐    WebSocket    ┌─────────────┐
│  ESP32-S3   │ ◄──────────────► │  FastAPI      │ ◄────────────► │  Browser     │
│  UART1      │   /ws/esp32      │  Server       │   /ws/frontend │  HTML page   │
│  TX39 RX38  │                  │  :8000        │                │              │
└─────────────┘                  └──────────────┘                └─────────────┘
```

## 文件结构

```
server/
├── main.py          # FastAPI WebSocket 服务器
├── static/
│   └── index.html   # 前端监视页面
├── requirements.txt # Python 依赖
└── deploy.sh        # 服务器部署脚本
```

## 服务器部署 (8.148.208.229)

```bash
# 1. 上传文件
scp -r server/* root@8.148.208.229:/www/wwwroot/esp32-bridge/

# 2. SSH 登录
ssh root@8.148.208.229

# 3. 运行部署脚本
cd /www/wwwroot/esp32-bridge
pip3 install -r requirements.txt
python3 main.py        # 前台测试运行

# 4. 后台服务 (可选)
bash deploy.sh         # 创建 systemd 服务，开机自启
```

访问：`http://8.148.208.229:8000`

## ESP32 编译和烧录

```bash
# 编译
idf.py build

# 烧录 (替换 COM 端口)
idf.py -p COM3 flash

# 监视串口输出
idf.py -p COM3 monitor
```

## 关键参数

| 参数 | 值 |
|------|-----|
| WiFi SSID | 0986 |
| WiFi 密码 | 12345678 |
| UART1 TX | GPIO 39 |
| UART1 RX | GPIO 38 |
| UART1 波特率 | 9600 / 8N1 |
| WebSocket 服务器 | 8.148.208.229:8000 |
| WS 路径 (ESP32) | /ws/esp32 |
| WS 路径 (前端) | /ws/frontend |

## UART0 命令

ESP32 上电后通过 UART0 串口可发送命令：

```
IP:8.148.208.229:8000     # 修改 WebSocket 服务器地址
WS=192.168.1.100:9000     # 同上
```
