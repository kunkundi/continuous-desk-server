#!/bin/bash
set -euo pipefail

# Environment variables for coturn
CONF_FILE=/etc/coturn/turnserver.conf
CERT_FILE=/opt/turnserver/turn_server_cert.pem
PKEY_FILE=/opt/turnserver/turn_server_pkey.pem

# Environment variables for crossdesk-server
CROSSDESK_SERVER_PORT=${CROSSDESK_SERVER_PORT:-9090}

# Create necessary directories
mkdir -p /etc/coturn /var/log/crossdesk

# Function to resolve hostname to IP address
# Tries multiple methods (getent, dig, nslookup) for portability
resolve_host() {
  local host="$1"
  local ip=""

  # Try getent first (most portable across Linux distributions)
  if command -v getent >/dev/null 2>&1; then
    ip=$(getent hosts "$host" | awk '{ print $1 }' | head -n 1)
  # Fallback to dig if available
  elif command -v dig >/dev/null 2>&1; then
    ip=$(dig +short "$host" | grep -E '^[0-9.]+$' | head -n 1)
  # Fallback to nslookup if available
  elif command -v nslookup >/dev/null 2>&1; then
    ip=$(nslookup "$host" | awk '/^Address: / { print $2 }' | tail -n 1)
  fi

  echo "$ip"
}

# Function to generate coturn configuration file
generate_coturn_config() {
  mkdir -p /etc/coturn
  cat > "$CONF_FILE" <<EOF
# Coturn auto-generated configuration
listening-port=${COTURN_PORT}
listening-ip=${INTERNAL_IP}
external-ip=${EXTERNAL_IP}
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
}

# Cleanup function for graceful shutdown
# Kills both services and waits for them to terminate
cleanup() {
  echo "Shutting down services gracefully..."

  # Kill monitoring process if it exists
  if [ -n "${MONITOR_PID:-}" ]; then
    kill $MONITOR_PID 2>/dev/null || true
  fi

  # Kill main services
  if [ -n "${COTURN_PID:-}" ]; then
    kill $COTURN_PID 2>/dev/null || true
  fi

  if [ -n "${CROSSDESK_PID:-}" ]; then
    kill $CROSSDESK_PID 2>/dev/null || true
  fi

  # Wait for processes to terminate
  wait $COTURN_PID $CROSSDESK_PID 2>/dev/null || true

  echo "All services stopped"
}

# Register cleanup function to run on EXIT, SIGTERM, and SIGINT
trap cleanup EXIT SIGTERM SIGINT

# Resolve EXTERNAL_HOST to IP if provided
# This allows using domain names instead of IPs
if [ -n "${EXTERNAL_HOST:-}" ]; then
  echo "Resolving EXTERNAL_HOST: $EXTERNAL_HOST"
  RESOLVED_IP=$(resolve_host "$EXTERNAL_HOST")

  if [ -z "$RESOLVED_IP" ]; then
    echo "Error: Failed to resolve EXTERNAL_HOST: $EXTERNAL_HOST"
    exit 1
  fi

  export EXTERNAL_IP="$RESOLVED_IP"
  echo "Resolved EXTERNAL_IP: $EXTERNAL_IP"
fi

# Validate required environment variables
if [ -z "${EXTERNAL_IP:-}" ] || [ -z "${INTERNAL_IP:-}" ]; then
  echo "Error: EXTERNAL_IP and INTERNAL_IP must be set."
  echo "Example: docker run -e EXTERNAL_IP=1.2.3.4 -e INTERNAL_IP=10.0.0.5 crossdesk-server"
  echo "Or use: docker run -e EXTERNAL_HOST=example.com -e INTERNAL_IP=10.0.0.5 crossdesk-server"
  exit 1
fi

if [ -z "${COTURN_PORT:-}" ]; then
  echo "Error: COTURN_PORT must be set."
  echo "Example: docker run -e COTURN_PORT=3478 crossdesk-server"
  exit 1
fi

if [ -z "${MIN_PORT:-}" ] || [ -z "${MAX_PORT:-}" ]; then
  echo "Error: MIN_PORT and MAX_PORT must be set."
  echo "Example: docker run -e MIN_PORT=50000 -e MAX_PORT=60000 crossdesk-server"
  exit 1
fi

# Check and generate certificates if needed
CERT_DIR="/var/lib/crossdesk/certs"
CERT_KEY="$CERT_DIR/api.crossdesk.cn.key"
CERT_BUNDLE="$CERT_DIR/api.crossdesk.cn_bundle.crt"
CERT_ROOT="$CERT_DIR/api.crossdesk.cn_root.crt"

if [ ! -f "$CERT_KEY" ] || [ ! -f "$CERT_BUNDLE" ]; then
  echo "Certificate files not found, generating certificates..."
  mkdir -p "$CERT_DIR"

  # Run certificate generation script with EXTERNAL_IP and output directory
  if ! bash /docker/generate_certs.sh "$EXTERNAL_IP" "$CERT_DIR"; then
    echo "Error: Certificate generation script failed"
    exit 1
  fi

  # Verify certificates were successfully generated
  if [ ! -f "$CERT_KEY" ] || [ ! -f "$CERT_BUNDLE" ] || [ ! -f "$CERT_ROOT" ]; then
    echo "Error: Failed to generate certificate files"
    exit 1
  fi

  echo "Certificates generated successfully"
else
  echo "Certificate files found, skipping generation"
fi

# Create symbolic links for coturn certificate paths
# This ensures coturn can find the certificates at the expected locations
mkdir -p /opt/turnserver
ln -sf "$CERT_BUNDLE" "$CERT_FILE"
ln -sf "$CERT_KEY" "$PKEY_FILE"

# Generate coturn configuration file
generate_coturn_config

echo "Generated coturn config at $CONF_FILE"
echo "Using certificate: $CERT_FILE"

# Start IP monitoring process if EXTERNAL_HOST is configured
# This monitors DNS changes and restarts services when the IP changes
if [ -n "${EXTERNAL_HOST:-}" ]; then
  {
    CHECK_INTERVAL=60  # Check every 60 seconds
    CURRENT_IP="$EXTERNAL_IP"

    while true; do
      sleep $CHECK_INTERVAL

      # Resolve hostname to check for IP changes
      NEW_IP=$(resolve_host "$EXTERNAL_HOST")

      # If IP has changed, restart services with new configuration
      if [ -n "$NEW_IP" ] && [ "$NEW_IP" != "$CURRENT_IP" ]; then
        echo "Detected IP change for $EXTERNAL_HOST: $CURRENT_IP -> $NEW_IP"
        echo "Restarting services with new IP..."

        # Gracefully terminate existing services using saved PIDs
        kill $COTURN_PID $CROSSDESK_PID 2>/dev/null || true
        wait $COTURN_PID $CROSSDESK_PID 2>/dev/null || true

        sleep 2

        # Update configuration with new IP
        export EXTERNAL_IP="$NEW_IP"
        CURRENT_IP="$NEW_IP"

        # Regenerate coturn configuration with new IP
        generate_coturn_config

        # Restart services
        turnserver -c "$CONF_FILE" &
        COTURN_PID=$!

        ./crossdesk-server/crossdesk_server ${CROSSDESK_SERVER_PORT} &
        CROSSDESK_PID=$!

        echo "Services restarted with new IP: $NEW_IP"
      fi
    done
  } &
  MONITOR_PID=$!
  echo "Started IP monitoring process (PID: $MONITOR_PID)"
fi

# Start coturn server in background
echo "Starting turnserver..."
turnserver -c "$CONF_FILE" &
COTURN_PID=$!

# Wait briefly and verify turnserver started successfully
sleep 2
if ! kill -0 $COTURN_PID 2>/dev/null; then
  echo "Error: turnserver failed to start"
  echo "Check logs at /var/log/crossdesk/turn.log for details"
  exit 1
fi
echo "Turnserver started successfully (PID: $COTURN_PID)"

# Start crossdesk-server in background
echo "Starting crossdesk-server..."
echo "Certificate directory: $CERT_DIR"
echo "Certificate files:"
ls -la "$CERT_DIR" || echo "Warning: Cannot list certificate directory"

./crossdesk-server/crossdesk_server ${CROSSDESK_SERVER_PORT} &
CROSSDESK_PID=$!

# Wait briefly and verify crossdesk-server started successfully
sleep 2
if ! kill -0 $CROSSDESK_PID 2>/dev/null; then
  echo "Error: crossdesk_server failed to start"
  kill $COTURN_PID 2>/dev/null || true
  exit 1
fi
echo "Crossdesk-server started successfully (PID: $CROSSDESK_PID)"

echo "All services running. Waiting for processes..."

# Wait for any process to exit
# The -n flag makes wait return when the first process exits
# This ensures the container exits if any critical service fails
wait -n

# Get the exit code of the first process that exited
EXIT_CODE=$?

echo "A service has exited with code $EXIT_CODE, shutting down container..."
exit $EXIT_CODE
