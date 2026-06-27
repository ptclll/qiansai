import os
from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse

app = FastAPI(title="地图标点工具", version="1.0.0")

# 获取 static 目录的绝对路径
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
STATIC_DIR = os.path.join(BASE_DIR, "static")

# 挂载静态文件（可选，用于直接访问静态资源）
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.get("/")
async def root():
    """返回主页（交互地图页面）"""
    return FileResponse(os.path.join(STATIC_DIR, "index.html"))


@app.get("/api/location")
async def get_default_location():
    """返回默认标点坐标"""
    return {
        "lng_dms": "112°28'41.4\"E",
        "lat_dms": "23°00'11.9\"N",
        "lng_dec": 112.478167,
        "lat_dec": 23.003306
    }


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
