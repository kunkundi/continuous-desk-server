#!/bin/bash
set -e

# environment variables for coturn
CONF_FILE=/etc/coturn/turnserver.conf
CERT_FILE=/opt/turnserver/turn_server_cert.pem
PKEY_FILE=/opt/turnserver/turn_server_pkey.pem

# environment variables for crossdesk-server
CROSSDESK_SERVER_PORT=${CROSSDESK_SERVER_PORT:-9090}
# Optional admin dashboard environment variables:
#   ADMIN_USERNAME and ADMIN_PASSWORD
# If both are set, crossdesk-server enables /admin on the HTTPS port. The
# values are inherited by the server process below.

is_uint() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

# check environment variables
if [ -z "$EXTERNAL_IP" ] || [ -z "$INTERNAL_IP" ]; then
  echo "Error: EXTERNAL_IP and INTERNAL_IP must be set."
  echo "EXTERNAL_IP may be a public IP (e.g. 1.2.3.4) or a domain name (e.g. desk.example.com)."
  echo "Example: docker run -e EXTERNAL_IP=desk.example.com -e INTERNAL_IP=10.0.0.5 crossdesk-server"
  exit 1
fi

# Resolve EXTERNAL_IP to a literal IPv4 address for coturn's external-ip.
# WebRTC ICE candidates require an IP, not a domain, so if EXTERNAL_IP is a
# domain name we resolve it once at startup via getent (glibc, always present
# on the ubuntu:22.04 base image). The original value is still passed to
# generate_certs.sh so the certificate gets a DNS: SAN for the domain.
is_ipv4() {
  [[ "$1" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]]
}

if is_ipv4 "$EXTERNAL_IP"; then
  EXTERNAL_IP_RESOLVED="$EXTERNAL_IP"
else
  EXTERNAL_IP_RESOLVED=$(getent ahostsv4 "$EXTERNAL_IP" 2>/dev/null | awk '{print $1; exit}')
  if [ -z "$EXTERNAL_IP_RESOLVED" ]; then
    echo "Error: Unable to resolve EXTERNAL_IP domain '$EXTERNAL_IP' to an IPv4 address."
    exit 1
  fi
  echo "Resolved EXTERNAL_IP domain '$EXTERNAL_IP' -> '$EXTERNAL_IP_RESOLVED'"
fi

if [ -z "$COTURN_PORT" ]; then
  echo "Error: COTURN_PORT must be set."
  echo "Example: docker run -e COTURN_PORT=3478 crossdesk-server"
  exit 1
fi

if [ -z "$MIN_PORT" ] || [ -z "$MAX_PORT" ]; then
  echo "Error: MIN_PORT and MAX_PORT must be set."
  echo "Example: docker run -e MIN_PORT=50000 -e MAX_PORT=60000 crossdesk-server"
  exit 1
fi

for port_name in CROSSDESK_SERVER_PORT COTURN_PORT MIN_PORT MAX_PORT; do
  port_value="${!port_name}"
  if ! is_uint "$port_value" || [ "$port_value" -lt 1 ] || [ "$port_value" -gt 65535 ]; then
    echo "Error: $port_name must be an integer between 1 and 65535."
    exit 1
  fi
done

if [ "$MIN_PORT" -gt "$MAX_PORT" ]; then
  echo "Error: MIN_PORT must be less than or equal to MAX_PORT."
  exit 1
fi

# check and generate certificates if needed
CERT_DIR="/var/lib/crossdesk/certs"
DB_DIR="/var/lib/crossdesk/db"
LOG_DIR="/var/log/crossdesk"
CERT_KEY="$CERT_DIR/api.crossdesk.cn.key"
CERT_BUNDLE="$CERT_DIR/api.crossdesk.cn_bundle.crt"
CERT_ROOT="$CERT_DIR/api.crossdesk.cn_root.crt"

mkdir -p "$CERT_DIR" "$DB_DIR" "$LOG_DIR"

if [ ! -f "$CERT_KEY" ] || [ ! -f "$CERT_BUNDLE" ]; then
  echo "Certificate files not found, generating certificates..."
  
  # Run generate_certs.sh with EXTERNAL_IP and output directory
  bash /docker/generate_certs.sh "$EXTERNAL_IP" "$CERT_DIR"
  
  # Verify certificates were generated
  if [ ! -f "$CERT_KEY" ] || [ ! -f "$CERT_BUNDLE" ] || [ ! -f "$CERT_ROOT" ]; then
    echo "Error: Failed to generate certificate files"
    exit 1
  fi
  
  echo "Certificates generated successfully"
else
  echo "Certificate files found, skipping generation"
fi

# generate coturn configuration file
mkdir -p /etc/coturn
cat > "$CONF_FILE" <<EOF
# coturn auto-generated configuration
listening-port=${COTURN_PORT}
listening-ip=${INTERNAL_IP}
external-ip=${EXTERNAL_IP_RESOLVED}
min-port=${MIN_PORT}
max-port=${MAX_PORT}
verbose
fingerprint
lt-cred-mech
user=crossdesk:crossdeskpw
realm=crossdesk
cert=${CERT_FILE}
pkey=${PKEY_FILE}
log-file=/var/log/crossdesk/turn.log
no-cli
EOF

echo "generated coturn config at $CONF_FILE"
echo "using certificate: $CERT_FILE"

# start coturn in the background
turnserver -c "$CONF_FILE" &

# start crossdesk-server as main foreground process
echo "Starting crossdesk-server..."
echo "Certificate directory: $CERT_DIR"
echo "Certificate files:"
ls -la "$CERT_DIR" || echo "Warning: Cannot list certificate directory"

exec ./crossdesk-server/crossdesk_server ${CROSSDESK_SERVER_PORT}
