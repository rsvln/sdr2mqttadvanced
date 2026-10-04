#!/usr/bin/bashio
CONFIG_PATH=/data/options.json

MQTT_HOST="$(bashio::config 'mqtt_host')"
MQTT_PORT="$(bashio::config 'mqtt_port')"
MQTT_USERNAME="$(bashio::config 'mqtt_user')"
MQTT_PASSWORD="$(bashio::config 'mqtt_password')"
MQTT_TOPIC="$(bashio::config 'mqtt_topic')"
MQTT_RETAIN="$(bashio::config 'mqtt_retain')"
PROTOCOL="$(bashio::config 'protocol')"
FREQUENCY="$(bashio::config 'frequency')"
UNITS="$(bashio::config 'units')"
DISCOVERY_PREFIX="$(bashio::config 'discovery_prefix')"
DISCOVERY_INTERVAL="$(bashio::config 'discovery_interval')"
WHITELIST_ENABLE="$(bashio::config 'whitelist_enable')"
WHITELIST="$(bashio::config 'whitelist')"
AUTO_DISCOVERY="$(bashio::config 'auto_discovery')"
DEBUG="$(bashio::config 'debug')"
EXPIRE_AFTER="$(bashio::config 'expire_after')"
NARTIS_SERIAL="$(bashio::config 'nartis_serial')"

# Exit immediately if a command exits with a non-zero status:
set -e

export LANG=C
PATH="/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin"
export LD_LIBRARY_PATH=/usr/local/lib64

# Start the listener and enter an endless loop
bashio::log.blue "::::::::Starting RTL_433 with parameters::::::::"
bashio::log.info "MQTT Host =" $MQTT_HOST
bashio::log.info "MQTT port =" $MQTT_PORT
bashio::log.info "MQTT User =" $MQTT_USERNAME
bashio::log.info "MQTT Password =" $(echo $MQTT_PASSWORD | sha256sum | cut -f1 -d' ')
bashio::log.info "MQTT Topic =" $MQTT_TOPIC
bashio::log.info "MQTT Retain =" $MQTT_RETAIN
bashio::log.info "PROTOCOL =" $PROTOCOL
bashio::log.info "FREQUENCY =" $FREQUENCY
bashio::log.info "Whitelist Enabled =" $WHITELIST_ENABLE
bashio::log.info "Whitelist =" $WHITELIST
bashio::log.info "Expire After =" $EXPIRE_AFTER
bashio::log.info "UNITS =" $UNITS
bashio::log.info "DISCOVERY_PREFIX =" $DISCOVERY_PREFIX
bashio::log.info "DISCOVERY_INTERVAL =" $DISCOVERY_INTERVAL
bashio::log.info "AUTO_DISCOVERY =" $AUTO_DISCOVERY
bashio::log.info "DEBUG =" $DEBUG
bashio::log.info "NARTIS_SERIAL =" $NARTIS_SERIAL
bashio::log.blue "::::::::rtl_433 running output::::::::"

MQTT_OUT="mqtt://$MQTT_HOST:$MQTT_PORT,user=$MQTT_USERNAME,pass=$MQTT_PASSWORD,retain=$MQTT_RETAIN,events=$MQTT_TOPIC/events,states=$MQTT_TOPIC/states,devices=$MQTT_TOPIC[/model][/id][/channel:A]"

if [ -z "$NARTIS_SERIAL" ] || [ "$NARTIS_SERIAL" = "null" ]; then
    rtl_433 $FREQUENCY $PROTOCOL -C $UNITS -F "$MQTT_OUT" -M time:tz:local -M protocol -M level | /scripts/rtl_433_mqtt_hass.py
else
    # Nartis D101/I300: the display and the meter use 4 channels over ~1.4 MHz.
    # Receive 2048k around 434 MHz; nartis_chanmix cuts out the Nartis channels plus
    # an aux channel for other ~433.92 MHz sensors and feeds one 256k stream to rtl_433.
    # The frequency option is ignored in this mode.
    NARTIS_NUM=$(rtl_433 -R help 2>&1 | grep -i 'Nartis' | sed -E 's/^[^0-9]*([0-9]+).*/\1/' | head -n1)
    if [ -z "$NARTIS_NUM" ]; then
        bashio::exit.nok "rtl_433 has no Nartis decoder"
    fi
    if [ -z "$PROTOCOL" ] || [ "$PROTOCOL" = "null" ]; then
        # "-R -N" registers all default decoders first, then ours is added
        PROTOCOL="-R -$NARTIS_NUM"
    fi
    PROTOCOL="$PROTOCOL -R $NARTIS_NUM:$NARTIS_SERIAL"
    bashio::log.info "Nartis mode: decoder #$NARTIS_NUM, protocols: $PROTOCOL"
    rtl_sdr -f 434000000 -s 2048000 - 2>/dev/null \
        | nartis_chanmix 434000000 433814000 433293600 434253400 434693600 433950000:aux \
        | rtl_433 -r cu8:- -s 256k $PROTOCOL -C $UNITS -F "$MQTT_OUT" -M time:tz:local -M protocol -M level \
        | /scripts/rtl_433_mqtt_hass.py
fi
