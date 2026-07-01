# ESP32-S3 WebSocket Serial Bridge

```
ESP32-S3 ──WiFi──► FastAPI Server ──WebSocket──► Browser
   │                 8.148.208.229:8000
   │
 UART1 (TX39 RX38 9600 8N1)
   │
 外部设备
```

## 快速开始

### 1. 服务器 (8.148.208.229)

```bash
scp -r server/* root@8.148.208.229:/www/wwwroot/esp32-bridge/
ssh root@8.148.208.229
cd /www/wwwroot/esp32-bridge
pip3 install -r requirements.txt
python3 main.py          # 或 bash deploy.sh 后台运行
```

浏览器打开 `http://8.148.208.229:8000`

### 2. ESP32

```bash
idf.py build
idf.py -p COM3 flash monitor
```

## 参数

| 项 | 值 |
|----|-----|
| WiFi | 0986 / 12345678 |
| UART1 TX | GPIO 39 |
| UART1 RX | GPIO 38 |
| UART1 波特率 | 9600, 8N1 |
| WS 服务器 | 8.148.208.229:8000 |

## UART0 命令

```
IP:x.x.x.x:port    # 修改 WebSocket 服务器地址
```

## Hardware

- Target: ESP32-S3
- UART1: TX=GPIO1, RX=GPIO2
- UART1: 115200, 8N1

## WiFi

- SSID: 357
- Password: 12345678

On connect, UART0 prints the IP and the HTTP URL.

## Build and Flash

```
idf.py -p PORT flash monitor
```

## Web Endpoints

- GET / : UI
- GET /data : latest UART1 RX text
- GET /config : button config as urlencoded text (btn1=...&btn2=...)
- POST /config : update button config (index=1..8&value=...)
- GET /send?index=1..8&mode=short|long : send button value to UART1
