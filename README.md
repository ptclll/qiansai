# qiansai

嵌入式大赛项目仓库，当前主要包含一个基于 ESP32-S3 的 WebSocket 串口桥接服务，用于把开发板串口数据实时转发到浏览器，并支持网页端反向发送数据到设备。

## 项目概述

本仓库用于嵌入式大赛相关开发，现阶段核心功能集中在 `iot_server/` 目录：

- ESP32-S3 通过 UART1 与外设/模块通信
- ESP32-S3 通过 WebSocket 将串口数据发送到服务器
- 服务器把数据广播到网页端实时监视页面
- 网页端也可以向 ESP32-S3 回传指令，完成双向调试

## 系统架构

```text
ESP32-S3 UART1  <──>  FastAPI WebSocket Server  <──>  Browser Dashboard
      TX39/RX38              /ws/esp32                 /ws/frontend
```

## 功能说明

### 1. ESP32-S3 串口桥接

- ESP32-S3 连接到 WebSocket 服务器 `/ws/esp32`
- 服务器接收来自串口的数据并广播给所有前端浏览器
- 浏览器输入的文本会转发回 ESP32-S3

### 2. 网页串口监视器

- 实时显示 UART 数据
- 支持清空日志、自动滚动
- 支持快捷发送常用指令，如 `AT`、`AT+GMR`、`AT+CWLAP`
- 在线状态轮询显示 ESP32 是否连接

### 3. 数据解析

当前前端对部分 JSON 数据做了专门展示：

- `type=gps`：显示 GPS 解析面板
- `type=mag` / `type=imu`：计算并显示方位角 `azimuth`

## 目录结构

```text
iot_server/
├── main.py          # FastAPI WebSocket 服务器
├── static/
│   └── index.html   # 前端监视页面
├── requirements.txt # Python 依赖
└── deploy.sh        # 服务器部署脚本
```

## 运行方式

### 服务器端

```bash
cd iot_server
pip3 install -r requirements.txt
python3 main.py
```

默认启动地址：`http://0.0.0.0:8010`

启动后可直接访问：

- `GET /`：网页监视界面
- `GET /health`：健康检查接口
- `WS /ws/esp32`：ESP32 连接入口
- `WS /ws/frontend`：网页端连接入口

### 后台部署

如果要在 Linux 服务器上长期运行，可使用 `deploy.sh` 创建 systemd 服务：

```bash
bash deploy.sh
```

## ESP32 参数

- UART1 TX：GPIO 39
- UART1 RX：GPIO 38
- 波特率：9600 / 8N1
- 前端页面会通过 WebSocket 与服务器保持连接，并自动重连

## 依赖

```text
fastapi
uvicorn
websockets
```

## 说明

- 当前仓库更偏向“比赛调试平台 + 实时通信中间层”
- 后续可以继续补充硬件接线图、ESP32 工程说明、协议格式和比赛方案介绍
- 如果你愿意，我也可以继续帮你把 README 补成“比赛答辩版”，写得更完整一点
