// gas-o-meter2.js
// Externe Konverter-Datei für Zigbee2MQTT zur Integration eines Custom ESP32C6 Gaszählers
// Kompatibel mit Zigbee2MQTT (getestet mit 2.7.2)
// Behebt den "UNSUPPORTED_ATTRIBUTE" Fehler durch manuelles Konfigurieren des Reportings.

const exposes = require('zigbee-herdsman-converters/lib/exposes');
const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const tz = require('zigbee-herdsman-converters/converters/toZigbee');
const ea = exposes.access;

const definition = {
    // Definieren Sie hier Ihr Modell und Ihren Hersteller, EXAKT wie in der ESP32C6 Firmware
    zigbeeModel: ['Gas-O-Meter2'], 
    model: 'gas-o-meter2',
    vendor: 'Custom',
    description: 'Custom Gas Meter with Battery status (ESP32C6) - Optimized Converter',
    
    fromZigbee: [
        // Ignoriert standardmäßige Basic Cluster Reports, da wir nur spezifische Daten verarbeiten
        fz.ignore_basic_report,

        // --- Gas Meter (Simple Metering Cluster 0x0702) ---
        {
            cluster: 'seMetering',
            type: ['attributeReport', 'readResponse'],
            convert: (model, msg, publish, options, meta) => {
                const result = {};
                // Multiplier/Divisor aus den Optionen lesen (werden in configure() gelesen und gespeichert)
                // Standardwerte: Multiplier=1, Divisor=100 (wie in C-Firmware zigbee_config.h)
                const multiplier = options && options.metering_multiplier !== undefined ? options.metering_multiplier : 1;
                const divisor = options && options.metering_divisor !== undefined ? options.metering_divisor : 100;

                // CurrentSummationDelivered verarbeiten (uint48)
                if (msg.data.currentSummDelivered !== undefined || msg.data.currentSummationDelivered !== undefined) {
                    const rawValue = msg.data.currentSummDelivered || msg.data.currentSummationDelivered;
                    let rawInt = 0n; // Verwende BigInt für die 48-Bit-Zahl

                    if (typeof rawValue === 'object' && rawValue !== null) {
                        // Verarbeitet verschiedene Objekt-/Array-Formate für uint48
                        if (Array.isArray(rawValue) && rawValue.length >= 2) {
                            rawInt = (BigInt(rawValue[0]) << 32n) | BigInt(rawValue[1]);
                        } else if (rawValue.low !== undefined && rawValue.high !== undefined) {
                            rawInt = (BigInt(rawValue.low) << 32n) | BigInt(rawValue.high);
                        }
                    } else {
                        // Fallback für einfache Number-Darstellung (weniger als 32 Bit)
                        rawInt = BigInt(rawValue);
                    }

                    // Skalierung: raw * multiplier / divisor (ergibt m³)
                    const scaledValue = Number(rawInt) * multiplier / divisor;
                    result.gas = scaledValue;
                }
                return Object.keys(result).length > 0 ? result : null;
            },
        },
        
        // --- Battery Status (Power Configuration Cluster 0x0001) ---
        {
            cluster: 'genPowerCfg',
            type: ['attributeReport', 'readResponse'],
            convert: (model, msg, publish, options, meta) => {
                const result = {};

                // Battery Percentage Remaining (Attributs-ID 0x0021)
                if (msg.data.batteryPercentageRemaining !== undefined) {
                    // Zigbee sendet 0-200 (für 0-100%). Z2M erwartet 0-100.
                    result.battery = Math.round(msg.data.batteryPercentageRemaining / 2);
                }

                // Battery Voltage (Attributs-ID 0x0020)
                if (msg.data.batteryVoltage !== undefined) {
                    // Zigbee sendet uint8 in 100mV Einheiten (z.B. 35 = 3.5V). Z2M erwartet mV (wie gewünscht).
                    result.voltage = msg.data.batteryVoltage * 100; // Umrechnung von 100mV in mV
                }

                // Battery Alarm State (Attributs-ID 0x003E)
                if (msg.data.batteryAlarmState !== undefined) {
                    // Bit 0 signalisiert "Low Voltage Alarm"
                    result.battery_low = (msg.data.batteryAlarmState & 0x01) !== 0;
                }
                return Object.keys(result).length > 0 ? result : null;
            },
        },
    ],
    
    toZigbee: [], // Keine 'toZigbee' Konvertierungen notwendig, da das Gerät nur Daten sendet

    // Definiert, welche Entitäten in Home Assistant erstellt werden sollen (Exposes)
    exposes: [
        // Gas-Zählerstand (continuous counter, wird in HA automatisch als 'gas' device_class erkannt)
        exposes.numeric('gas', ea.STATE)
            .withUnit('m³')
            .withDescription('Gas consumption in m³. Cumulative counter.')
            .withCategory('diagnostic'),

        // Batteriestand in Prozent
        exposes.numeric('battery', ea.STATE)
            .withUnit('%')
            .withDescription('Remaining battery in %')
            .withValueMin(0)
            .withValueMax(100)
            .withCategory('diagnostic'),

        // Batteriespannung in Millivolt (mV)
        exposes.numeric('voltage', ea.STATE)
            .withUnit('mV')
            .withDescription('Battery Volts (mV)')
            .withCategory('diagnostic'),

        // Batteriewarnung (boolean: true bei Alarm, false sonst)
        exposes.binary('battery_low', ea.STATE, true, false)
            .withDescription('Battery Low (boolean)')
            .withCategory('diagnostic'),
    ],
    
    // Die Configure-Funktion wird nach dem Pairing ausgeführt, um das Reporting einzustellen.
    configure: async (device, coordinatorEndpoint, logger) => {
        const endpoint = device.getEndpoint(1);

        // Lese Multiplier/Divisor vom Gerät und speichere sie in den Z2M-Optionen (für fromZigbee Konverter)
        try {
            const data = await endpoint.read('seMetering', ['multiplier', 'divisor']);
            device.saveDeviceOptions({
                metering_multiplier: data.multiplier,
                metering_divisor: data.divisor,
            });
        } catch (error) {
            // Ignorieren falls nicht lesbar
        }
        
        // --- Reporting Konfiguration (behebt UNSUPPORTED_ATTRIBUTE Fehler aus dem Log) ---
        // Wir konfigurieren explizit nur die Attribute, die das ESP32C6 unterstützt.

        // Konfigurieren von Metering Reporting: CurrentSummationDelivered (0x0000)
        // Das Attribut 'instantaneousDemand' (0x0400) wird ignoriert, da es den Fehler verursachte.
        try {
            await endpoint.configureReporting('seMetering', [{
                attribute: 'currentSummDelivered', 
                minimumReportInterval: 1, // Mindestintervall 1s
                maximumReportInterval: 65000, // Maximalintervall (ca. 18h)
                reportableChange: 1, // Melden bei einer Änderung von 1 raw unit
            }]);
        } catch (error) { /* Ignorieren falls Reporting nicht konfigurierbar */ }

        // Battery Percentage Reporting
        try {
            await endpoint.configureReporting('genPowerCfg', [{
                attribute: 'batteryPercentageRemaining',
                minimumReportInterval: 3600, // 1h
                maximumReportInterval: 21600, // 6h
                reportableChange: 1 // 1% Änderung
            }]);
        } catch (error) { /* Ignorieren */ }

        // Battery Voltage Reporting
        try {
            await endpoint.configureReporting('genPowerCfg', [{
                attribute: 'batteryVoltage',
                minimumReportInterval: 3600, // 1h
                maximumReportInterval: 21600, // 6h
                reportableChange: 10 // 1V Änderung (100mV Einheiten)
            }]);
        } catch (error) { /* Ignorieren */ }

        // Battery Alarm State Reporting
        try {
            await endpoint.configureReporting('genPowerCfg', [{
                attribute: 'batteryAlarmState',
                minimumReportInterval: 3600, // 1h
                maximumReportInterval: 21600, // 6h
            }]);
        } catch (error) { /* Ignorieren */ }

        logger.info(`Successfully configured device ${device.ieeeAddr}`);
    },
    meta: {
        configureKey: 1, // Stellt sicher, dass die Konfiguration bei jedem Join/Reboot versucht wird
        // Verhindert, dass Zigbee2MQTT automatisch modernExtend verwendet
        disableDefaultResponse: true,
    },
};

module.exports = definition;
