ARG BUILD_FROM=ghcr.io/home-assistant/amd64-base:latest
FROM $BUILD_FROM

# Define environment variables
# Use this variable when creating a container to specify the MQTT broker host.
ENV MQTT_HOST=127.0.0.1 \
    MQTT_PORT=1883 \
    MQTT_USERNAME="" \
    MQTT_PASSWORD="" \
    MQTT_RETAIN=True \
    MQTT_TOPIC=rtl_433 \
    FREQUENCY=433.92M \
    PROTOCOL="" \
    WHITELIST_ENABLE=False \
    EXPIRE_AFTER=0 \
    WHITELIST="" \
    DISCOVERY_PREFIX=homeassistant \
    DISCOVERY_INTERVAL=600 \
    AUTO_DISCOVERY=False \
    DEBUG=False \
    LANG=C.UTF-8

LABEL Maintainer="rsvln (fork of Jeffrey Stone's sdr2mqtt)" \
Description="RTL433 to HASS via MQTT, with the Nartis D101/I300 electricity meter decoder"

# rtl_433 fork with the Nartis decoder
ARG RTL433_REPO=https://github.com/rsvln/rtl_433
ARG RTL433_REF=nartis-d101

WORKDIR /data

# Copy scripts
COPY entry.sh rtl_433_mqtt_hass.py nartis_chanmix.c /scripts/
# install rtl-sdr, libusb, mosquitto-clients, python3 py3-paho-mqtt; build rtl_433 from the fork and nartis_chanmix.
# make entry.sh and rtl_433_mqtt_hass.py executable.
RUN apk add --no-cache rtl-sdr libusb mosquitto-clients python3 py3-paho-mqtt && \
    apk add --no-cache --virtual .build-deps build-base cmake git librtlsdr-dev libusb-dev && \
    git clone --depth 1 --branch "$RTL433_REF" "$RTL433_REPO" /tmp/rtl_433 && \
    cmake -S /tmp/rtl_433 -B /tmp/rtl_433/build -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_SOAPYSDR=OFF -DENABLE_OPENSSL=OFF -DBUILD_TESTING=OFF && \
    make -C /tmp/rtl_433/build -j"$(nproc)" install && \
    gcc -O2 -o /usr/local/bin/nartis_chanmix /scripts/nartis_chanmix.c -lm && \
    apk del .build-deps && \
    rm -rf /tmp/rtl_433 && \
    chmod +x /scripts/entry.sh && \
    chmod +x /scripts/rtl_433_mqtt_hass.py

# Execute entry script
ENTRYPOINT [ "/scripts/entry.sh" ]
