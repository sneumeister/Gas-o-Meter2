#!/usr/bin/env python3
"""Simuliert transfer_mqtt_publish_ha_discovery JSON (nach Firmware-Stand)."""
import json

EXPIRE_AFTER_SEC = 120 * 60

def build_payload(main_topic: str, hostname: str, tail: str, component: str) -> str:
    slug = main_topic.replace("/", "_").replace("-", "_").replace(" ", "") or "device"
    unique_id = f"gas_o_meter2_{slug}_{tail}"
    state_data = f"{main_topic}/data"
    state_rssi = f"{main_topic}/rssi"
    state_ntp = f"{main_topic}/ntp_status"
    state_status = f"{main_topic}/status"
    expire = f'"expire_after":{EXPIRE_AFTER_SEC},'
    avail = (
        f'"availability_topic":"{state_status}",'
        f'"payload_available":"online","payload_not_available":"offline"'
    )
    ident = f"gas_o_meter2_{slug}"
    device = (
        f',"device":{{"identifiers":["{ident}"],'
        f'"name":"{hostname}","manufacturer":"Custom","model":"Gas-O-Meter2",'
        f'"sw_version":"1.0.0"}}'
    )
    bodies = {
        "gas": (
            '{"name":"Gas Counter","unique_id":"%s","state_topic":"%s",%s'
            '"value_template":"{{ value_json.gas }}","unit_of_measurement":"m\\u00b3",'
            '"device_class":"gas","state_class":"total_increasing","icon":"mdi:meter-gas",%s%s}'
        ),
        "battery_low": (
            '{"name":"Battery Low","unique_id":"%s","state_topic":"%s",%s'
            '"value_template":"{{%% if value_json.battery_low %%}}ON{{%% else %%}}OFF{{%% endif %%}}",'
            '"payload_on":"ON","payload_off":"OFF","device_class":"battery",'
            '"icon":"mdi:battery-alert",%s%s}'
        ),
        "ntp_status": (
            '{"name":"Last NTP Sync","unique_id":"%s","state_topic":"%s",%s'
            '"device_class":"timestamp","value_template":"{{ as_datetime(value) }}",'
            '"entity_category":"diagnostic",%s%s}'
        ),
    }
    state_topics = {
        "gas": state_data,
        "battery_low": state_data,
        "ntp_status": state_ntp,
    }
    body = bodies[tail] % (unique_id, state_topics[tail], expire, avail, device)
    topic = f"homeassistant/{component}/{unique_id}/config"
    return topic, body

def main():
    main_topic = "gas-o-meterTEST"
    hostname = "gas-o-meter2"
    for tail, comp in [("gas", "sensor"), ("battery_low", "binary_sensor"), ("ntp_status", "sensor")]:
        topic, body = build_payload(main_topic, hostname, tail, comp)
        print("TOPIC:", topic)
        try:
            json.loads(body)
            print("JSON: OK")
        except json.JSONDecodeError as e:
            print("JSON: FAIL", e)
        idx = body.find("offline")
        print("SNIP:", body[idx : idx + 30])
        print()

if __name__ == "__main__":
    main()
