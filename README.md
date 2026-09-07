# CrossDesk Server

为 [CrossDesk](https://github.com/kunkundi/crossdesk) 提供设备注册、在线状态、远程会话信令与临时 TURN 凭据，内置 Web 管理后台。信令使用 WSS，设备资料与时长统计使用 SQLite 持久化。

[English](README_EN.md) · [下载发布配置](https://github.com/kunkundi/crossdesk-server/releases) · [Docker 镜像](https://hub.docker.com/r/crossdesk/crossdesk-server/tags) · [客户端](https://github.com/kunkundi/crossdesk) · [Web 客户端](https://github.com/kunkundi/crossdesk-web-client)

[![Build](https://github.com/kunkundi/crossdesk-server/actions/workflows/build.yml/badge.svg)](https://github.com/kunkundi/crossdesk-server/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/kunkundi/crossdesk-server)](https://github.com/kunkundi/crossdesk-server/releases)
[![Docker Pulls](https://img.shields.io/docker/pulls/crossdesk/crossdesk-server)](https://hub.docker.com/r/crossdesk/crossdesk-server)
[![License: LGPL v3](https://img.shields.io/badge/license-LGPL--3.0-blue)](LICENSE)

[部署服务](#运行服务) · [连接客户端](#clients) · [管理后台](#admin) · [状态接口](#stats) · [维护与排查](#operations) · [源码构建](#build)

## 组件与能力

| 组件 | 职责 |
| --- | --- |
| CrossDesk Server | WSS 信令、设备身份与密码验证、在线状态、会话协调、临时 TURN 凭据 |
| Coturn | 在客户端无法直连时中继媒体与数据；由独立容器运行 |
| SQLite | 保存设备记录、在线与远控时长及会话信息 |
| Web 管理后台 | 查看指标、客户端与活动会话，搜索 / 筛选 / 排序，断开选定会话，可选 IP 地域分布 |

客户端优先尝试 P2P 直连，必要时通过 TURN 中继。当前 [Compose](compose.yaml) 和 [CI](.github/workflows/build.yml) 面向 **Linux amd64 / arm64**；发布镜像以 Ubuntu 22.04 为运行环境。

## 运行服务

### 1. 准备部署配置

在已安装 **Docker Engine 和 Compose 插件**的 Linux 服务器上操作，先确认 `docker compose version` 可用。Compose 使用主机网络；`INTERNAL_IP` 必须是宿主机实际可绑定的地址。

从同一个 [GitHub Release](https://github.com/kunkundi/crossdesk-server/releases) 下载 **`compose.yaml`** 和 **`env.example`**，放在单独的部署目录中。在该目录执行：

```bash
cp env.example .env
```

Release 附件中的 `env.example` 已固定对应镜像 tag。源码仓库中的文件名是 [`.env.example`](.env.example)，其镜像值为 `latest`；使用它时请自行选择固定发布 tag。后续 Compose 命令都在配置目录执行。

### 2. 编辑 `.env`

用文本编辑器填写以下配置，替换所有示例地址。云服务器存在公网 NAT 时，公网和内网 IP 通常不同；公网 IP 直接配置在网卡上时，两项可以相同。

| 配置 | 用途 / 默认值 |
| --- | --- |
| `CROSSDESK_IMAGE` | 使用与部署文件配套的固定发布 tag |
| `EXTERNAL_IP` | 公网 IP，用于默认 TURN 地址与自动证书生成 |
| `INTERNAL_IP` | Coturn 监听和中继使用的本机网卡 IP |
| `CROSSDESK_SERVER_PORT` | WSS / HTTPS 共用端口，Compose 默认 `9099` |
| `COTURN_PORT` | STUN / TURN 端口，默认 `3478` |
| `MIN_PORT` / `MAX_PORT` | TURN 媒体端口范围，默认 `50000`–`60000` |
| `COTURN_AUTH_SECRET` | 信令服务与 Coturn 共享的签名密钥，不填入客户端 |
| `COTURN_STATELESS_NONCE_SECRET` | 独立的 nonce 密钥，生成后持久保存 |
| `CROSSDESK_DATA_DIR` | 数据与证书的宿主机目录，默认 `/var/lib/crossdesk` |
| `CROSSDESK_LOG_DIR` | 业务日志的宿主机目录，默认 `/var/log/crossdesk` |

分别运行两次下面的命令，将两个不同的输出填入上述两个密钥字段：

```bash
openssl rand -hex 32
openssl rand -hex 32
```

可选项 `COTURN_PUBLIC_HOST` 用于向客户端公布独立的 TURN 域名 / IP，留空时使用 `EXTERNAL_IP`。`COTURN_CREDENTIAL_TTL_SECONDS` 默认 `3600` 秒，范围为 `60`–`86400`；`COTURN_LOG_LEVEL` 默认 `warning`，`COTURN_MEMORY_LIMIT` 默认 `512m`。完整参数见 [环境变量示例](.env.example)。

客户端登录和会话协商时由信令服务签发临时 TURN 用户名、密码；不要在客户端或 Web 页面中配置服务器共享密钥。更新服务端时同时核对客户端的版本兼容性。

### 3. 放通端口并启动

防火墙及云安全组需要允许：

| 端口 | 协议 | 用途 |
| --- | --- | --- |
| `CROSSDESK_SERVER_PORT`，默认 `9099` | TCP | WSS、状态接口与管理后台 |
| `COTURN_PORT`，默认 `3478` | TCP / UDP | STUN / TURN |
| `MIN_PORT`–`MAX_PORT`，默认 `50000`–`60000` | UDP | TURN 中继媒体 |

保存 `.env` 后执行：

```bash
sudo docker compose config -q
sudo docker compose pull
sudo docker compose up -d --no-build
sudo docker compose ps
```

`crossdesk_server` 会首次生成证书并创建数据库；`crossdesk_coturn` 等待证书文件就绪后启动。Compose 的健康检查只确认密钥和证书文件存在，还需通过下文的状态接口及实际客户端连接验证服务。

本文连接示例均使用 Compose 默认端口 **9099**；如果修改了端口，后续 URL 和客户端配置也要一起修改。直接运行二进制且不传端口参数时，程序默认使用 **9090**。

<a id="clients"></a>

## 证书与客户端连接

### 信任自己的服务器证书

默认文件位于 `${CROSSDESK_DATA_DIR}/certs/`：

| 文件 | 用途 |
| --- | --- |
| `api.crossdesk.cn_root.crt` | 分发到客户端的根证书 |
| `api.crossdesk.cn_bundle.crt` | 服务端证书链 |
| `api.crossdesk.cn.key` | 服务端私钥，保留在服务器 |

这些是程序约定的**文件名**，不要求服务器使用 `api.crossdesk.cn` 域名。[自动生成脚本](docker/generate_certs.sh) 只将 `EXTERNAL_IP` 写入证书的 IP 地址范围；客户端应使用该 IP。使用自定义域名时，需提供包含该域名的有效证书链和私钥，分别放到 `bundle.crt` 和 `.key` 对应的上述路径。

[启动脚本](docker/start.sh) 会复用已有证书，修改 `.env` 中的 IP 不会重新签发证书。重新生成自签证书会更换根证书，所有客户端都需重新信任。使用受系统信任的 CA 证书时通常无需额外导入根证书。

将自己部署的 `api.crossdesk.cn_root.crt` 复制到客户端后，按平台导入系统信任库：

```powershell
# Windows：管理员 PowerShell，替换为实际文件路径
certutil -addstore "Root" "C:\path\to\api.crossdesk.cn_root.crt"
```

```bash
# Ubuntu / Debian
sudo cp /path/to/api.crossdesk.cn_root.crt /usr/local/share/ca-certificates/
sudo update-ca-certificates
```

```bash
# macOS
sudo security add-trusted-cert -d -r trustRoot \
  -k /Library/Keychains/System.keychain /path/to/api.crossdesk.cn_root.crt
```

### 配置控制端和被控端

1. 在桌面客户端打开 **☰ → 设置 → 自托管配置**。
2. 填写服务器 IP / 域名、信令端口 `9099`、中继端口 `3478`，点击“确认”。地址栏不填协议前缀、路径或端口。
3. 返回设置，勾选“自托管配置”，再点击“确认”保存。两端都需使用同一服务器。
4. 完成证书信任后重新打开客户端，等待“已连接服务器”，使用被控端当前显示的 ID 和密码连接。

当前桌面客户端通过**系统信任库**验证证书，无需在自托管窗口选择证书文件。切换服务器后请重新核对设备 ID。

- **Web：** 在 [Web 客户端](https://github.com/kunkundi/crossdesk-web-client) 的 `web_client.js` 中配置 `signalingUrl`（例如 `wss://203.0.113.10:9099`）及 STUN 地址（例如 `stun:203.0.113.10:3478`），替换示例 IP；浏览器也需信任证书，被控端保持 SRTP 开启。
- **原生 iOS：** 在“设置 → 服务器”中填写信令主机、信令端口与 STUN/TURN 端口并应用；自签根证书需要在设备上安装并启用信任。

<a id="admin"></a>

## 后台管理

在 `.env` 中同时设置 `ADMIN_USERNAME` 和 `ADMIN_PASSWORD`，然后重新执行 `sudo docker compose up -d --no-build`。只设置其中一项不会启用后台。浏览器访问 **`https://服务器地址:9099/admin`**，信任证书后使用该账户登录。

| 区域 / 入口 | 操作 |
| --- | --- |
| 顶部指标 | 查看在线设备、Web 客户端、活动连接与累计在线 / 远控时长 |
| Client Presence | 默认显示在线 PC；可切换 PC / Web，筛选 Online / Remote / Offline / All，搜索 ID、排序和翻页 |
| Details | 展开版本、平台、当前与累计时长、连接 IP、地域及远控对端 |
| Active Sessions → Disconnect | 确认后断开所选会话；设备本身保持在线 |
| Refresh lists / Logout | 手动刷新列表 / 退出管理登录 |

页面可见时每 5 秒刷新数据，时长每秒更新显示。管理登录会话保存在内存中，默认有效期 8 小时；服务重启后需要重新登录。后台使用 HTTPS 登录 Cookie。

**可选地域分布：** 默认关闭 IP 地理查询。在 `.env` 中启用 `CROSSDESK_GEOIP_LOOKUP=1` 并填写 `CROSSDESK_GEOIP_KEY` 后，公网连接 IP 会发送给 IP2Location 查询国家和省 / 州。图表统计当前在线客户端，包含 Web、排除 `C-*` 控制分身；未解析位置单独统计。关闭查询或无可用结果时，地图不会显示已解析的分布。

连接 IP 和地域只作为在线状态保存在内存，不写入设备资料数据库。后台保留 IP2Location 归因链接；中国地图资源 [china-provinces.json](src/admin/web/china-provinces.json) 来自 ISC 许可的 `china-map-geojson@1.0.4`。

<details>
<summary>地域查询与后台资源的高级配置</summary>

- `CROSSDESK_GEOIP_SCHEME`、`CROSSDESK_GEOIP_HOST`、`CROSSDESK_GEOIP_PORT`、`CROSSDESK_GEOIP_PATH` 配置查询端点；默认 HTTPS、`api.ip2location.io`、`443`、`/?key={key}&ip={ip}`。
- `CROSSDESK_GEOIP_TIMEOUT_MS` 默认 `1200`；失败重试由 `CROSSDESK_GEOIP_FAILURE_TTL_MS` 和 `CROSSDESK_GEOIP_FAILURE_MAX_TTL_MS` 控制，默认从 `60000` 毫秒退避至 `1800000` 毫秒。
- 成功结果缓存；失败按 IP 去重并退避重试，该 IP 已无在线设备时停止重试。
- 前端源码位于 [src/admin/web](src/admin/web)，容器内为 `/crossdesk-server/admin`。自定义 `CROSSDESK_ADMIN_WEB_DIR` 时，还需在 Compose 的 `environment` 中显式传入并挂载对应目录；仅添加到 `.env` 不会自动传入容器。修改前端资源后重启服务以刷新资源缓存。

</details>

<a id="stats"></a>

## 服务状态接口

`GET /stats` 和 `GET /api/stats` 与 WSS 共用 HTTPS 端口，**无需管理登录**，返回汇总统计并允许跨域读取。可在服务器上使用以下命令检查；替换示例 IP，若调整了数据目录也需修改根证书路径：

```bash
curl --fail --cacert /var/lib/crossdesk/certs/api.crossdesk.cn_root.crt \
  https://203.0.113.10:9099/stats
```

使用受信任 CA 的域名证书时：

```bash
curl --fail https://your-domain.example.com:9099/stats
```

| 字段 | 含义 |
| --- | --- |
| `online_device_count` | 当前在线设备数，排除 `web-*` 与 `C-*` |
| `online_web_client_count` | 当前在线 `web-*` 客户端数 |
| `active_connection_count` | 活动远控连接数，按 host 与各 guest 的连接分别计数 |
| `online_duration_seconds` | 当前在线设备的本次在线时长合计 |
| `total_online_seconds` | 设备累计在线时长，包含当前在线时段 |
| `total_control_seconds` / `total_controlled_seconds` | 累计控制 / 被控制时长，包含进行中的会话 |

时长单位为秒。管理后台的明细接口使用 `/api/admin/*`，需要管理登录；公共状态接口不会返回设备明细。

<a id="operations"></a>

## 维护与排查

### 查看日志与更新

在配置目录执行：

```bash
sudo docker compose ps
sudo docker compose logs --tail 100 crossdesk-server coturn
```

更新时先备份，再修改 `.env` 中的固定镜像 tag，并使用与该版本匹配的部署文件：

```bash
sudo docker compose config -q
sudo docker compose pull
sudo docker compose up -d --no-build
```

`docker compose restart` 用于重启现有容器，不会应用修改后的环境变量或镜像。业务日志写入 `${CROSSDESK_LOG_DIR}` 并同时输出到控制台；Coturn 日志通过 `docker compose logs coturn` 查看。两个容器的控制台日志均按 3 个、每个 50 MB 轮转。业务文件日志另行轮转，多次重启会产生新的日志组，需定期清理或归档。

### 备份与迁移

备份 `.env`、`compose.yaml` 及 `${CROSSDESK_DATA_DIR}` 下的 `certs/` 和 `db/`；备份包含密钥与设备数据库。以下示例使用默认数据目录，在停止服务后打包，期间远程连接会中断：

```bash
sudo docker compose stop
backup_file="crossdesk-backup-$(date +%Y%m%d-%H%M%S).tar.gz"
sudo tar -czf "$backup_file" .env compose.yaml -C /var/lib/crossdesk certs db
sudo docker compose up -d --no-build
```

迁移时在目标服务器恢复配套文件，核对挂载目录和权限，再启动。公网 IP / 域名改变时，还需更新证书与客户端地址。旧部署使用自定义数据目录时，把 `.env` 的 `CROSSDESK_DATA_DIR` 和 `CROSSDESK_LOG_DIR` 指向原有目录。

### 常见问题

| 现象 | 检查方向 |
| --- | --- |
| 配置校验失败 | 必填 IP 和两个密钥是否为空，配置文件是否来自同一 Release |
| 证书生成 / 数据库写入失败 | 查看信令日志，确认挂载路径可写；默认镜像以 root 启动，只有修改容器用户等情况下才按实际 UID/GID 调整权限 |
| Coturn 无法绑定地址 | `INTERNAL_IP` 是否属于宿主机网卡，端口是否被其他进程占用 |
| 客户端 TLS 错误 | 系统根证书信任、证书有效期及 IP / 域名匹配；修改 IP 不会更新已有证书 |
| 已连服务器但对端离线 | 两端是否使用同一服务，被控端是否运行，是否复制了当前 ID |
| P2P 失败且无法中继 | 客户端中继选项、客户端版本、共享密钥一致性、TURN 与媒体端口是否放通 |
| 后台未启用 / 登录失败 | 两个管理环境变量是否均已传入，是否重新创建容器，是否使用 HTTPS |
| 地图无分布 | 地理查询是否启用、API key 和出站网络是否可用；默认关闭查询 |

<a id="build"></a>

## 从源码构建

CI 在 **Ubuntu 22.04、amd64 / arm64** 上构建。安装 Git、C++17 编译工具链和 [Xmake](https://xmake.io/guide/quick-start.html)；第三方 C++ 依赖由 Xmake 下载。下面的 Linux 依赖命令与 CI 基线一致：

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates git curl unzip build-essential

git clone https://github.com/kunkundi/crossdesk-server.git
cd crossdesk-server
xmake f -c -m release -y
xmake b -vy crossdesk_server
```

首次构建前需确保 `xmake --version` 可用。其他系统的源码配置见 [xmake.lua](xmake.lua)，当前发布流程只提供 Linux 二进制与容器镜像。切换 Debug 使用 `xmake f -m debug`；`xmake r -d crossdesk_server` 表示使用调试器运行。

### 构建自己的运行镜像

Dockerfile **只打包已编译的 Linux 二进制**。先在与目标镜像架构相同的 Ubuntu 22.04 环境完成 Release 编译，再在仓库根目录执行：

```bash
# amd64；arm64 将 x86_64 替换为 arm64
install -Dm755 build/linux/x86_64/release/crossdesk_server dist/crossdesk_server
sudo docker build -f docker/dockerfile -t crossdesk-server:local .
```

在仓库根目录将 `.env.example` 复制为 `.env`，按部署章节填写配置，并将 `CROSSDESK_IMAGE` 设为 `crossdesk-server:local`。然后执行：

```bash
sudo docker compose config -q
sudo docker compose pull coturn
sudo docker compose up -d --no-build
```

直接运行二进制时，需要先准备 [main.cpp](src/main.cpp) 规定的 `/var/lib/crossdesk/certs`、数据库和日志路径；`CROSSDESK_DATA_DIR` / `CROSSDESK_LOG_DIR` 是 Compose 的宿主机挂载设置，不是二进制的路径覆盖参数。二进制不会自动生成证书。

### CI 测试镜像

默认分支成功构建后发布多架构 `crossdesk/crossdesk-server:test`，该 tag 随后续构建变化。测试环境可在 `.env` 中设置 `CROSSDESK_IMAGE=crossdesk/crossdesk-server:test`，再执行部署章节的拉取与启动命令。正式部署使用固定发布 tag。发布产物与镜像流程见 [CI 工作流](.github/workflows/build.yml)。

## 反馈与许可

[提交问题](https://github.com/kunkundi/crossdesk-server/issues)时请附上服务端 tag、两端客户端版本、部署方式及脱敏日志。CrossDesk Server 使用 [LGPL-3.0](LICENSE) 许可。
