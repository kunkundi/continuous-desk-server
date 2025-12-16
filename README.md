# CrossDesk Server

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-brightgreen.svg)]()
[![License: LGPL v3](https://img.shields.io/badge/License-LGPL%20v3-blue.svg)](https://www.gnu.org/licenses/lgpl-3.0)
[![GitHub last commit](https://img.shields.io/github/last-commit/kunkundi/crossdesk-server)](https://github.com/kunkundi/crossdesk-server/commits/web-client)
[![Build Status](https://github.com/kunkundi/crossdesk-server/actions/workflows/build.yml/badge.svg)](https://github.com/kunkundi/crossdesk/actions)  
[![Docker Pulls](https://img.shields.io/docker/pulls/crossdesk/crossdesk-server)](https://hub.docker.com/r/crossdesk/crossdesk-server/tags)
[![GitHub issues](https://img.shields.io/github/issues/kunkundi/crossdesk-server.svg)]()
[![GitHub stars](https://img.shields.io/github/stars/kunkundi/crossdesk-server.svg?style=social)]()
[![GitHub forks](https://img.shields.io/github/forks/kunkundi/crossdesk-server.svg?style=social)]()

[ [English](README_EN.md) / 中文 ]

为 [CrossDesk](https://github.com/kunkundi/crossdesk) 设计的服务端，支持WSS加密连接，使用SQLite3存储用户信息。

---

## 如何编译

依赖：
- [xmake](https://xmake.io/#/guide/installation)

编译
```
git clone https://github.com/kunkundi/crossdesk-server.git

cd crossdesk-server

xmake b crossdesk_server
```

## 关于 Xmake
#### 编译选项
```
# 切换编译模式
xmake f -m debug/release

# 可选编译参数
-r ：重新构建目标
-v ：显示详细的构建日志
-y ：自动确认提示

# 示例
xmake b -vy crossdesk_server
```
更多使用方法可参考 [Xmake官方文档](https://xmake.io/guide/quick-start.html) 。

## 构建镜像
```
cd docker

sudo docker build -t image-name .
```

## 运行容器

### 启动命令
```bash
sudo docker run -d \
  --name crossdesk_server \
  --network host \
  -e EXTERNAL_IP=xxx.xxx.xxx.xxx \
  -e INTERNAL_IP=xxx.xxx.xxx.xxx \
  -e CROSSDESK_SERVER_PORT=xxxx \
  -e COTURN_PORT=xxxx \
  -e MIN_PORT=xxxxx \
  -e MAX_PORT=xxxxx \
  -v /var/lib/crossdesk:/var/lib/crossdesk \
  -v /var/log/crossdesk:/var/log/crossdesk \
  crossdesk/crossdesk-server:v1.1.3
```

上述命令中，用户需注意的参数如下：

**参数**
- EXTERNAL_IP：服务器公网 IP，对应 CrossDesk 客户端**自托管服务器配置**中填写的**服务器地址**
- EXTERNAL_HOST：服务器域名（可选），用于替代 EXTERNAL_IP。设置后会自动解析域名为 IP 地址，并在域名 IP 变化时自动更新配置并重启服务。适用于动态 IP 或使用 DDNS 的场景
- INTERNAL_IP：服务器内网 IP
- CROSSDESK_SERVER_PORT：自托管服务使用的端口，对应 CrossDesk 客户端**自托管服务器配置**中填写的**服务器端口**
- COTURN_PORT：COTURN 服务使用的端口，对应 CrossDesk 客户端**自托管服务器配置**中填写的**中继服务端口**
- MIN_PORT/MAX_PORT：COTURN 服务使用的端口范围，例如：MIN_PORT=50000, MAX_PORT=60000，范围可根据客户端数量调整
- `-v /var/lib/crossdesk:/var/lib/crossdesk`：持久化数据库和证书文件到宿主机
- `-v /var/log/crossdesk:/var/log/crossdesk`：持久化日志文件到宿主机

**注意**：EXTERNAL_IP 和 EXTERNAL_HOST 二选一即可，优先推荐使用 EXTERNAL_HOST（支持自动 IP 更新）

**示例 1：使用固定 IP 地址**
```bash
sudo docker run -d \
  --name crossdesk_server \
  --network host \
  -e EXTERNAL_IP=114.114.114.114 \
  -e INTERNAL_IP=10.0.0.1 \
  -e CROSSDESK_SERVER_PORT=9099 \
  -e COTURN_PORT=3478 \
  -e MIN_PORT=50000 \
  -e MAX_PORT=60000 \
  -v /var/lib/crossdesk:/var/lib/crossdesk \
  -v /var/log/crossdesk:/var/log/crossdesk \
  crossdesk/crossdesk-server:v1.1.3
```

**示例 2：使用域名（推荐，支持动态 IP）**
```bash
sudo docker run -d \
  --name crossdesk_server \
  --network host \
  -e EXTERNAL_HOST=example.com \
  -e INTERNAL_IP=10.0.0.1 \
  -e CROSSDESK_SERVER_PORT=9099 \
  -e COTURN_PORT=3478 \
  -e MIN_PORT=50000 \
  -e MAX_PORT=60000 \
  -v /var/lib/crossdesk:/var/lib/crossdesk \
  -v /var/log/crossdesk:/var/log/crossdesk \
  crossdesk/crossdesk-server:v1.1.3
```

**注意**：
- **服务器需开放端口：COTURN_PORT/udp，COTURN_PORT/tcp，MIN_PORT-MAX_PORT/udp，CROSSDESK_SERVER_PORT/tcp。**
- 如果不挂载 volume，容器删除后数据会丢失
- 证书文件会在首次启动时自动生成并持久化到宿主机的 `/var/lib/crossdesk/certs` 路径下
- 数据库文件会自动创建并持久化到宿主机的 `/var/lib/crossdesk/db/crossdesk-server.db` 路径下
- 日志文件会自动创建并持久化到宿主机的 `/var/log/crossdesk/` 路径下

**权限注意**：如果 Docker 自动创建的目录权限不足（属于 root），容器内用户无法写入，会导致：
  - 证书生成失败，容器启动脚本会报错退出
  - 数据库目录创建失败，程序会抛出异常并崩溃
  - 日志目录创建失败，日志文件无法写入（但程序可能继续运行）
  
**解决方案**：在启动容器前手动设置权限：
```bash
sudo mkdir -p /var/lib/crossdesk /var/log/crossdesk
sudo chown -R $(id -u):$(id -g) /var/lib/crossdesk /var/log/crossdesk
```

## 证书文件
在宿主机的 `/var/lib/crossdesk/certs` 路径下可找到证书文件 `crossdesk.cn_root.crt`，下载到你的客户端主机，并在客户端的**自托管服务器设置**中选择相应的**证书文件路径**。