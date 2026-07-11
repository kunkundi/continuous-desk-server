#!/bin/bash
set -e

# 检查参数
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "Usage: $0 <SERVER_ADDR> [OUTPUT_DIR]"
    echo "  SERVER_ADDR: public IP address or domain name for the certificate"
    echo "  OUTPUT_DIR: Directory to save certificates (default: current directory)"
    exit 1
fi

SERVER_IP="$1"
OUTPUT_DIR="${2:-$(pwd)}"

# 确保输出目录存在
mkdir -p "$OUTPUT_DIR"

# 切换到输出目录
cd "$OUTPUT_DIR"

# 文件名（使用完整路径）
ROOT_KEY="$OUTPUT_DIR/api.crossdesk.cn_root.key"
ROOT_CERT="$OUTPUT_DIR/api.crossdesk.cn_root.crt"
SERVER_KEY="$OUTPUT_DIR/api.crossdesk.cn.key"
SERVER_CSR="$OUTPUT_DIR/api.crossdesk.cn.csr"
SERVER_CERT="$OUTPUT_DIR/api.crossdesk.cn_bundle.crt"
FULLCHAIN_CERT="$OUTPUT_DIR/api.crossdesk.cn_fullchain.crt"
SAN_CONF="$OUTPUT_DIR/san.cnf"

# 证书主题
SUBJ="/C=CN/ST=Zhejiang/L=Hangzhou/O=CrossDesk/OU=CrossDesk/CN=$SERVER_IP"

# Determine whether SERVER_ADDR is an IPv4 address or a domain name, and build
# the appropriate subjectAltName value. WebRTC/TURN clients validate the cert
# SAN, so for a domain we emit DNS:<domain> (and also the resolved IP when the
# domain can be resolved at cert-generation time, so connecting by IP still works).
if [[ "$SERVER_IP" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]]; then
  # Literal IPv4 address
  SAN_VALUE="IP:$SERVER_IP"
else
  # Treat as a domain name / hostname
  SAN_VALUE="DNS:$SERVER_IP"
  RESOLVED_IP=$(getent ahostsv4 "$SERVER_IP" 2>/dev/null | awk '{print $1; exit}')
  if [ -n "$RESOLVED_IP" ]; then
    SAN_VALUE="$SAN_VALUE,IP:$RESOLVED_IP"
  fi
fi

# 1. 生成根证书
echo "Generating root private key..."
openssl genrsa -out "$ROOT_KEY" 4096

echo "Generating self-signed root certificate..."
openssl req -x509 -new -nodes -key "$ROOT_KEY" -sha256 -days 3650 -out "$ROOT_CERT" -subj "$SUBJ"

# 2. 生成服务器私钥
echo "Generating server private key..."
openssl genrsa -out "$SERVER_KEY" 2048

# 3. 生成服务器 CSR
echo "Generating server CSR..."
openssl req -new -key "$SERVER_KEY" -out "$SERVER_CSR" -subj "$SUBJ"

# 4. 生成临时 OpenSSL 配置文件，加入 SAN
cat > "$SAN_CONF" <<EOL
[ req ]
default_bits = 2048
distinguished_name = req_distinguished_name
req_extensions = req_ext
prompt = no

[ req_distinguished_name ]
C = CN
ST = Zhejiang
L = Hangzhou
O = CrossDesk
OU = CrossDesk
CN = $SERVER_IP

[ req_ext ]
subjectAltName = $SAN_VALUE
EOL

# 5. 用根证书签发服务器证书（包含 SAN）
echo "Signing server certificate with root certificate..."
openssl x509 -req -in "$SERVER_CSR" -CA "$ROOT_CERT" -CAkey "$ROOT_KEY" -CAcreateserial \
  -out "$SERVER_CERT" -days 3650 -sha256 -extfile "$SAN_CONF" -extensions req_ext

# 6. 生成完整链证书并更新 bundle.crt（包含服务器证书和根证书）
cat "$SERVER_CERT" "$ROOT_CERT" > "$FULLCHAIN_CERT"
# 将完整证书链写入 bundle.crt，这样服务器可以使用完整的证书链
cp "$FULLCHAIN_CERT" "$SERVER_CERT"

# 7. 清理中间文件
rm -f "$ROOT_CERT.srl" "$SAN_CONF" "$ROOT_KEY" "$SERVER_CSR" "$FULLCHAIN_CERT"

echo "Generation complete. Certificates saved to: $OUTPUT_DIR"
echo "  Client root certificate: $ROOT_CERT"
echo "  Server private key: $SERVER_KEY"
echo "  Server certificate: $SERVER_CERT"