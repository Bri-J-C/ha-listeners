#!/usr/bin/with-contenv bashio

set -e

# Get config values
export MQTT_HOST=$(bashio::config 'mqtt_host')
export MQTT_PORT=$(bashio::config 'mqtt_port')
export MQTT_USER=$(bashio::config 'mqtt_user')
export MQTT_PASSWORD=$(bashio::config 'mqtt_password')
export DEVICE_NAME=$(bashio::config 'device_name')
export MULTICAST_GROUP=$(bashio::config 'multicast_group')
export MULTICAST_PORT=$(bashio::config 'multicast_port')
# Piper settings with defaults
if bashio::config.exists 'piper_host'; then
  export PIPER_HOST=$(bashio::config 'piper_host')
else
  export PIPER_HOST="core-piper"
fi
if bashio::config.exists 'piper_port'; then
  export PIPER_PORT=$(bashio::config 'piper_port')
else
  export PIPER_PORT="10200"
fi

# Log level
if bashio::config.exists 'log_level'; then
  export LOG_LEVEL=$(bashio::config 'log_level')
else
  export LOG_LEVEL="info"
fi

echo "Starting Intercom Hub..."
echo "MQTT: ${MQTT_HOST}:${MQTT_PORT}"
echo "Multicast: ${MULTICAST_GROUP}:${MULTICAST_PORT}"
echo "Checking Python..."

python3 --version

echo "Starting Python script..."
exec python3 -u /intercom_hub.py
