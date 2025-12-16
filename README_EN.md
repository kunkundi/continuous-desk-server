# CrossDesk Server

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-brightgreen.svg)]()
[![License: LGPL v3](https://img.shields.io/badge/License-LGPL%20v3-blue.svg)](https://www.gnu.org/licenses/lgpl-3.0)
[![GitHub last commit](https://img.shields.io/github/last-commit/kunkundi/crossdesk-server)](https://github.com/kunkundi/crossdesk-server/commits/web-client)
[![Build Status](https://github.com/kunkundi/crossdesk-server/actions/workflows/build.yml/badge.svg)](https://github.com/kunkundi/crossdesk/actions)  
[![Docker Pulls](https://img.shields.io/docker/pulls/crossdesk/crossdesk-server)](https://hub.docker.com/r/crossdesk/crossdesk-server/tags)
[![GitHub issues](https://img.shields.io/github/issues/kunkundi/crossdesk-server.svg)]()
[![GitHub stars](https://img.shields.io/github/stars/kunkundi/crossdesk-server.svg?style=social)]()
[![GitHub forks](https://img.shields.io/github/forks/kunkundi/crossdesk-server.svg?style=social)]()

[ [中文](README.md) / English ]

Server designed for [CrossDesk](https://github.com/kunkundi/crossdesk) , supporting WSS-encrypted connections and using SQLite3 to store user information.

---

Requirements:
- [xmake](https://xmake.io/#/guide/installation)

Build:
```
git clone https://github.com/kunkundi/crossdesk-server.git

cd crossdesk-server

xmake b crossdesk_server
```

## About Xmake
#### Build Options
```
# Switch build mode
xmake f -m debug/release

# Optional build parameters
-r : Rebuild the target
-v : Show detailed build logs
-y : Automatically confirm prompts

# Example
xmake b -vy crossdesk_server
```
For more information, please refer to the [official Xmake documentation](https://xmake.io/guide/quick-start.html) .

## Build Docker Image
```
cd docker

sudo docker build -t image-name .
```

## Run Container

### Startup Command
```
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

The parameters you need to pay attention to are as follows:

**Parameters**
- **EXTERNAL_IP**: The server's public IP. This corresponds to **Server Address** in the CrossDesk client's **Self-Hosted Server Configuration**.
- **EXTERNAL_HOST**: The server's domain name (optional), used instead of EXTERNAL_IP. When set, it automatically resolves the domain to an IP address and automatically updates configuration and restarts services when the domain's IP changes. Ideal for dynamic IP or DDNS scenarios.
- **INTERNAL_IP**: The server's internal IP.
- **CROSSDESK_SERVER_PORT**: The port used by the self-hosted service. This corresponds to **Server Port** in the CrossDesk client's **Self-Hosted Server Configuration**.
- **COTURN_PORT**: The port used by the COTURN service. This corresponds to **Relay Service Port** in the CrossDesk client's **Self-Hosted Server Configuration**.
- **MIN_PORT / MAX_PORT**: The port range used by the COTURN service. Example: `MIN_PORT=50000`, `MAX_PORT=60000`. Adjust the range depending on the number of clients.
- `-v /var/lib/crossdesk:/var/lib/crossdesk`: Persists database and certificate files on the host machine.
- `-v /var/log/crossdesk:/var/log/crossdesk`: Persists log files on the host machine.

**Note**: Use either EXTERNAL_IP or EXTERNAL_HOST. EXTERNAL_HOST is recommended for automatic IP updates.

**Example 1: Using Static IP Address**
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

**Example 2: Using Domain Name (Recommended for Dynamic IP)**
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

**Notes**
- **The server must open the following ports: COTURN_PORT/udp, COTURN_PORT/tcp, MIN_PORT–MAX_PORT/udp, and CROSSDESK_SERVER_PORT/tcp.**
- If you don’t mount volumes, all data will be lost when the container is removed.
- Certificate files will be automatically generated on first startup and persisted to the host at `/var/lib/crossdesk/certs`.
- The database file will be automatically created and stored at `/var/lib/crossdesk/db/crossdesk-server.db`.
- Log files will be created and stored at `/var/log/crossdesk/`.

**Permission Notice**
If the directories automatically created by Docker belong to root and have insufficient write permissions, the container user may not be able to write to them. This can cause:
  - Certificate generation failure, leading to startup script errors and container exit.
  - Database directory creation failure, causing the program to throw exceptions and crash.
  - Log directory creation failure, preventing logs from being written (though the program may continue running).

**Solution:** Manually set permissions before starting the container:
```bash
sudo mkdir -p /var/lib/crossdesk /var/log/crossdesk
sudo chown -R $(id -u):$(id -g) /var/lib/crossdesk /var/log/crossdesk
```

### Certificate Files
You can find the certificate file `crossdesk.cn_root.crt` at `/var/lib/crossdesk/certs` on the host machine.
Download it to your client device and select it in the **Certificate File Path** field under the CrossDesk client’s **Self-Hosted Server Settings**.