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
- EXTERNAL_IP：服务器公网 IP **或域名**，对应 CrossDesk 客户端**自托管服务器配置**中填写的**服务器地址**。填域名（如 `desk.example.com`）时，容器启动时会自动把域名解析为 IP 注入 TURN 的 `external-ip`（ICE 候选仍需 IP），证书则以 `DNS:` 形式写入 SAN，客户端据此信任该域名。
- INTERNAL_IP：服务器内网 IP（绑定地址，必须是 IP，不能是域名）
- CROSSDESK_SERVER_PORT：自托管服务使用的端口，对应 CrossDesk 客户端**自托管服务器配置**中填写的**服务器端口**
- COTURN_PORT: COTURN 服务使用的端口, 对应 CrossDesk 客户端**自托管服务器配置**中填写的**中继服务端口**
- MIN_PORT/MAX_PORT：COTURN 服务使用的端口范围，例如：MIN_PORT=50000, MAX_PORT=60000，范围可根据客户端数量调整。
- `-v /var/lib/crossdesk:/var/lib/crossdesk`：持久化数据库和证书文件到宿主机
- `-v /var/log/crossdesk:/var/log/crossdesk`：持久化日志文件到宿主机

**示例**：
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

### 使用域名（EXTERNAL_IP 支持域名）

`EXTERNAL_IP` 既可以是公网 IP（如 `114.114.114.114`），也可以是域名（如 `desk.example.com`）。启用域名时：

- **证书**：启动时自动生成的 TLS 证书会把域名写入 `subjectAltName` 的 `DNS:` 条目（并尽量附带解析出的公网 IP），客户端据此信任该域名，无需再手动信任 IP。
- **TURN / ICE**：WebRTC 的 ICE 候选地址必须是 IP，因此容器启动时会用 `getent` 把域名解析为 IP，注入 coturn 的 `external-ip`；中继流量仍走该公网 IP。
- **客户端**：CrossDesk 客户端「自托管服务器配置 → 服务器地址」直接填该域名即可（客户端本就支持域名，会自动 DNS 解析连信令）。

#### 免重建镜像部署（推荐先这样验证）

仓库内置 `docker-compose.yml`，把修改后的 `docker/start.sh` 与 `docker/generate_certs.sh` 直接挂载进官方镜像的固定路径（`/start.sh`、`/docker/generate_certs.sh`），**无需重新构建镜像**即可使用域名能力：

```bash
# 修改 docker-compose.yml 中的 EXTERNAL_IP / INTERNAL_IP 后
docker compose up -d
```

注意：

- 从「仅 IP」切换为「域名」后，需删除已持久化的证书目录（默认 `./crossdesk-data/certs`）再 `up`，才会以 `DNS:` SAN 重新生成证书（`start.sh` 发现证书已存在会跳过生成）。
- 挂载的脚本文件必须是 **LF 换行**（CRLF 在 Linux 容器内会报 `bad interpreter`）。
- 若从源码 `docker build` 构建镜像，域名支持已包含在 `docker/start.sh` 与 `docker/generate_certs.sh` 中，正常构建即可。

## 服务状态接口

服务启动后，可通过同一 HTTPS 端口读取运行状态：

官方 CA 部署示例：

```bash
curl https://your-domain.example.com:9090/stats
```

自签证书部署示例：

```bash
curl --cacert /var/lib/crossdesk/certs/api.crossdesk.cn_root.crt \
  https://your-server-ip:9090/stats
```

说明：
- 官方 CA 证书通常已被系统信任，`curl` 无需额外指定 `--cacert`
- 自签证书需要显式指定根证书 `api.crossdesk.cn_root.crt`
- 请求地址必须与服务端证书中的域名或 IP 一致，不能随意替换为 `127.0.0.1`

也支持路径 `/api/stats`，返回示例：

```json
{
  "online_device_count": 12,
  "online_web_client_count": 2,
  "active_connection_count": 3,
  "online_duration_seconds": 86400,
  "total_online_seconds": 259200,
  "total_control_seconds": 3600,
  "total_controlled_seconds": 7200
}
```

- `online_device_count`：当前在线设备数，不包含临时 `web-*` 客户端和 `C-*` 分身客户端
- `online_web_client_count`：当前在线 Web 客户端数，仅统计临时 `web-*` 客户端
- `active_connection_count`：当前处于连接中的会话数，按各 host 当前连接的 guest 数汇总；guest 自身登录或加入产生的连接不另行计数
- `online_duration_seconds`：当前在线设备本次在线时长的总和，不包含临时 `web-*` 客户端和 `C-*` 分身客户端
- `total_online_seconds`：设备累计在线时长的总和，包含当前仍在线设备的本次在线时长
- `total_control_seconds`：设备作为控制端的累计远控时长总和，包含当前仍在进行的远控会话
- `total_controlled_seconds`：设备作为被控端的累计远控时长总和，包含当前仍在进行的远控会话
- 响应已带 `Access-Control-Allow-Origin: *`，可直接被网页端 `fetch` 调用

## 证书文件
如果使用项目自带的自签证书方案，可在宿主机的 `/var/lib/crossdesk/certs` 路径下找到根证书 `api.crossdesk.cn_root.crt`，下载到你的客户端主机，并在客户端的**自托管服务器设置**中选择相应的**证书文件路径**。

如果使用官方 CA 证书，则通常不需要单独分发上述根证书，客户端和 `curl` 会直接使用系统信任链校验证书。

## 后台管理页面

服务端可以在同一个 HTTPS 端口提供内置后台管理页面，访问路径为 `/admin`。

启动前同时设置以下两个环境变量即可启用后台管理：

```bash
ADMIN_USERNAME=admin
ADMIN_PASSWORD=change-this-password
```

Docker 示例：

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
  -e ADMIN_USERNAME=admin \
  -e ADMIN_PASSWORD=change-this-password \
  -v /var/lib/crossdesk:/var/lib/crossdesk \
  -v /var/log/crossdesk:/var/log/crossdesk \
  crossdesk/crossdesk-server:v1.1.3
```

启动后打开：

```text
https://your-domain.example.com:9090/admin
```

后台页面会显示在线设备数、在线 Web 客户端数、活动远控会话、累计在线时长、累计控制时长和累计被控时长，并支持断开选中的远控会话。页面包含中国用户分布地图，可用颜色深浅查看各省份用户数量，国外用户会单独汇总展示。客户端状态列表默认展示在线 PC 客户端，并提供在线、远控中、离线、全部状态筛选以及 PC/Web 客户端类别选择；列表支持搜索、分页、排序和展开详情，并显示当前在线连接 IP 解析出的地理位置。客户端 IP 和地理位置只作为在线状态保存在内存中，不作为设备资料持久化到数据库；客户端在线时本次在线时长会实时刷新，下线后保留记录并显示最后在线时间点。

公网 IP 的地理位置默认不访问外部服务，内网、回环和 Docker 私有网段会显示为 `Private network`。如需开启公网 GeoIP 查询，可设置 `CROSSDESK_GEOIP_LOOKUP=1` 并通过 `CROSSDESK_GEOIP_KEY` 配置 IP2Location API key；默认请求 `https://api.ip2location.io/?key={key}&ip={ip}`。解析只读取 `country_name`、`country_code` 和 `region_name`，并拼出类似 `California, United States of America` 的位置文本，不再读取或展示城市。地域统计只有在国家和可识别省份都缺失时才计入未解析。使用 IP2Location.io 免费计划或无 key API 时需要展示归因，后台地域分布区域会显示 `CrossDesk uses IP2Location.io IP geolocation web service.` 并链接到 `https://www.ip2location.io`。查询端点可通过 `CROSSDESK_GEOIP_SCHEME`、`CROSSDESK_GEOIP_HOST`、`CROSSDESK_GEOIP_PORT` 和 `CROSSDESK_GEOIP_PATH` 配置，其中路径里的 `{ip}` 和 `{key}` 会被替换。查询超时时间可通过 `CROSSDESK_GEOIP_TIMEOUT_MS` 调整，默认 1200ms。成功 IP 结果会缓存；失败结果不作为设备位置缓存，而是由后台 IP 队列按 IP 去重并按退避重新入队。重试时如果已没有在线设备使用该 IP，任务会直接丢弃。退避默认从 60000ms 开始翻倍，最高 1800000ms，可通过 `CROSSDESK_GEOIP_FAILURE_TTL_MS` 和 `CROSSDESK_GEOIP_FAILURE_MAX_TTL_MS` 调整。

后台前端资源位于 `src/admin/web`，容器内默认复制到 `/crossdesk-server/admin`；如需使用自定义前端目录，可设置 `CROSSDESK_ADMIN_WEB_DIR`。中国地图边界数据 `china-provinces.json` 由 ISC 许可的 `china-map-geojson@1.0.4` 省级 GeoJSON 数据生成。
