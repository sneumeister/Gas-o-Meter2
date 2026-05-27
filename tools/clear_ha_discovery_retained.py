#!/usr/bin/env python3
"""
Löscht retained Home-Assistant-MQTT-Discovery-Configs für gas-o-meter2.

Sendet pro Topic einen leeren Payload mit retain=True (HA-Standard zum Entfernen).

Beispiele:
  pip install paho-mqtt
  python tools/clear_ha_discovery_retained.py --host 192.168.1.10 --main-topic gas-o-meterTEST
  python tools/clear_ha_discovery_retained.py --host broker.local --dry-run
  python tools/clear_ha_discovery_retained.py --scan --host 192.168.1.10
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("Bitte installieren: pip install paho-mqtt", file=sys.stderr)
    sys.exit(1)

HA_PREFIX = "homeassistant"
DEVICE_PREFIX = "gas_o_meter2"

ENTRIES = (
    ("sensor", "gas"),
    ("sensor", "battery"),
    ("sensor", "voltage"),
    ("binary_sensor", "battery_low"),
    ("sensor", "firmware"),
    ("sensor", "rssi"),
    ("sensor", "ntp_status"),
)


def slug_current(main_topic: str) -> str:
    """Wie mqtt_ha_main_topic_slug() in transfer_mqtt.cpp (/, -, Leerzeichen)."""
    out: list[str] = []
    for ch in main_topic:
        if ch in "/-":
            out.append("_")
        elif ch != " ":
            out.append(ch)
    s = "".join(out)
    return s if s else "device"


def slug_legacy(main_topic: str) -> str:
    """Alter Slug: nur / → _, Bindestriche bleiben (Topics vor Firmware-Fix)."""
    out: list[str] = []
    for ch in main_topic:
        if ch == "/":
            out.append("_")
        elif ch != " ":
            out.append(ch)
    s = "".join(out)
    return s if s else "device"


def unique_id(slug: str, tail: str) -> str:
    return f"{DEVICE_PREFIX}_{slug}_{tail}"


def discovery_topic(component: str, uid: str) -> str:
    return f"{HA_PREFIX}/{component}/{uid}/config"


def topics_for_main_topic(main_topic: str) -> list[str]:
    slugs = {slug_current(main_topic), slug_legacy(main_topic)}
    topics: list[str] = []
    for component, tail in ENTRIES:
        for slug in sorted(slugs):
            topics.append(discovery_topic(component, unique_id(slug, tail)))
    return topics


def load_config_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as f:
        return json.load(f)


def main() -> int:
    parser = argparse.ArgumentParser(description="Retained HA-Discovery-Topics löschen (gas-o-meter2)")
    parser.add_argument("--host", help="MQTT-Broker (oder mqtt_host aus --config)")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--username", default="")
    parser.add_argument("--password", default="")
    parser.add_argument(
        "--main-topic",
        action="append",
        dest="main_topics",
        metavar="TOPIC",
        help="mqtt_main_topic (mehrfach möglich, z.B. gas-o-meterTEST und gas-o-meter2)",
    )
    parser.add_argument(
        "--config",
        type=Path,
        help="config.json vom Gerät (liest mqtt_host, mqtt_port, mqtt_main_topic)",
    )
    parser.add_argument(
        "--scan",
        action="store_true",
        help="Zusätzlich homeassistant/# abonnieren und alle gas_o_meter2_* /config löschen",
    )
    parser.add_argument("--scan-seconds", type=float, default=3.0, help="Dauer --scan (Sekunden)")
    parser.add_argument("--dry-run", action="store_true", help="Nur anzeigen, nichts senden")
    args = parser.parse_args()

    host = args.host
    port = args.port
    username = args.username
    password = args.password
    main_topics: list[str] = list(args.main_topics or [])

    if args.config:
        if not args.config.is_file():
            print(f"Config nicht gefunden: {args.config}", file=sys.stderr)
            return 1
        cfg = load_config_json(args.config)
        host = host or cfg.get("mqtt_host")
        port = cfg.get("mqtt_port", port)
        username = username or cfg.get("mqtt_username", "")
        password = password or cfg.get("mqtt_password", "")
        mt = cfg.get("mqtt_main_topic")
        if mt and mt not in main_topics:
            main_topics.append(mt)

    if not host or host == "dummy_mqtt_host":
        print("MQTT-Host fehlt (--host oder --config mit echtem mqtt_host).", file=sys.stderr)
        return 1

    if not main_topics and not args.scan:
        main_topics = ["gas-o-meterTEST", "gas-o-meter2"]

    topics: set[str] = set()
    for mt in main_topics:
        topics.update(topics_for_main_topic(mt))

    scanned: set[str] = set()
    if args.scan:

        def on_message(_client, _userdata, msg) -> None:
            t = msg.topic
            if not t.endswith("/config"):
                return
            if DEVICE_PREFIX not in t:
                return
            if not t.startswith(f"{HA_PREFIX}/"):
                return
            scanned.add(t)

        scan_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        if username:
            scan_client.username_pw_set(username, password or None)
        try:
            scan_client.connect(host, port, keepalive=30)
        except Exception as exc:
            print(f"Verbindung fehlgeschlagen: {exc}", file=sys.stderr)
            return 1
        scan_client.on_message = on_message
        scan_client.subscribe(f"{HA_PREFIX}/#", qos=0)
        scan_client.loop_start()
        time.sleep(max(0.5, args.scan_seconds))
        scan_client.loop_stop()
        scan_client.disconnect()
        topics |= scanned

    topics_sorted = sorted(topics)
    if not topics_sorted:
        print("Keine Topics zum Löschen.")
        return 0

    print(f"Broker: {host}:{port}")
    print(f"Topics ({len(topics_sorted)}):")
    for t in topics_sorted:
        print(f"  {t}")

    if args.dry_run:
        print("\nDry-run – nichts gesendet.")
        return 0

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    if username:
        client.username_pw_set(username, password or None)
    try:
        client.connect(host, port, keepalive=30)
    except Exception as exc:
        print(f"Verbindung fehlgeschlagen: {exc}", file=sys.stderr)
        return 1

    client.loop_start()
    ok = 0
    for topic in topics_sorted:
        info = client.publish(topic, payload=b"", qos=1, retain=True)
        info.wait_for_publish(timeout=5.0)
        if info.rc == mqtt.MQTT_ERR_SUCCESS:
            ok += 1
            print(f"  gelöscht: {topic}")
        else:
            print(f"  FEHLER ({info.rc}): {topic}", file=sys.stderr)
    time.sleep(0.3)
    client.loop_stop()
    client.disconnect()

    print(f"\nFertig: {ok}/{len(topics_sorted)} retained Discovery-Configs geleert.")
    print("Danach in HA: MQTT-Integration neu laden.")
    return 0 if ok == len(topics_sorted) else 1


if __name__ == "__main__":
    sys.exit(main())
