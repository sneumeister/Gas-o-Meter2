// gas-o-meter2.js
// 
// Externe Konverter-Datei für Zigbee2MQTT zur Integration eines Custom ESP32C6 Gaszählers
// 
// Getestet mit:
// - Zigbee2MQTT Version: 2.8.0 (bzw. 2.8.0-1)
// - zigbee-herdsman / zigbee-herdsman-converters: mit Z2M 2.8.x installiert
//
// Beschreibung:
// Dieser Konverter verarbeitet Daten vom Simple Metering Cluster (0x0702) für Gaszählung
// und vom Power Configuration Cluster (0x0001) für Batteriestatus.
// Behebt den "UNSUPPORTED_ATTRIBUTE" Fehler durch manuelles Konfigurieren des Reportings.
//
// Hardware: Custom ESP32C6 Zigbee Gas Meter
// Firmware: ESP-Zigbee-SDK mit Multiplier=1, Divisor=100

// Benötigte Module aus zigbee-herdsman-converters
const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const e = exposes.presets;      // Vordefinierte Exposes (z.B. battery, voltage)
const ea = exposes.access;      // Access-Level (STATE, SET, etc.)

// Lokale fromZigbee Konverter - verarbeiten eingehende Zigbee-Nachrichten
const fzLocal = {
    // Gas Meter Konverter für Simple Metering Cluster (0x0702)
    gas_metering: {
        cluster: 'seMetering',  // Simple Metering Cluster
        type: ['attributeReport', 'readResponse'],  // Reagiert auf Reports und Read-Antworten
        convert: (model, msg, publish, options, meta) => {
            // Prüfe ob currentSummDelivered (Attribut 0x0000) vorhanden ist
            if (msg.data.hasOwnProperty('currentSummDelivered')) {
                // Umrechnung: raw_value / divisor (100) = m³
                // ESP32C6 sendet raw_value, Z2M/HA erwartet m³
                return {gas: msg.data['currentSummDelivered'] / 100};
            }
            // Wenn kein relevantes Attribut vorhanden, leeres Objekt zurückgeben
            return {};
        },
    },
    
    // Custom Battery Konverter mit battery_voltage statt voltage
    battery_custom: {
        cluster: 'genPowerCfg',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            const result = {};
            
            // Battery Percentage Remaining (Attribut 0x0021)
            // Zigbee sendet 0-200 (für 0-100%), Z2M erwartet 0-100
            if (msg.data.hasOwnProperty('batteryPercentageRemaining')) {
                result.battery = msg.data['batteryPercentageRemaining'] / 2;
            }
            
            // Battery Voltage (Attribut 0x0020) als battery_voltage in V (float)
            // Zigbee sendet uint8 in 100mV-Einheiten (z.B. 42 = 4.2V)
            if (msg.data.hasOwnProperty('batteryVoltage')) {
                result.battery_voltage = parseFloat((msg.data['batteryVoltage'] / 10).toFixed(1));
            }
            
            // Battery Alarm State (Attribut 0x003E)
            // Bit 0 signalisiert "Low Voltage Alarm"
            if (msg.data.hasOwnProperty('batteryAlarmState')) {
                result.battery_low = (msg.data['batteryAlarmState'] & 0x01) !== 0;
            }
            
            return result;
        },
    },
};

// Haupt-Definition des Geräts
const definition = {
    // Muss EXAKT mit dem zigbeeModel in der ESP32C6 Firmware übereinstimmen
    zigbeeModel: ['Gas-O-Meter2'],
    
    // Modell-ID für Zigbee2MQTT (wird in Logs und Frontend angezeigt)
    model: 'gas-o-meter2',
    
    // Hersteller-Name
    vendor: 'Custom',
    
    // Beschreibung des Geräts
    description: 'Custom Gas Meter with Battery (ESP32C6)',

    // Z2M/herdsman: Semver 0.0.<patch> (Default 0.0.0). Patch erhoehen → Geraet wird neu konfiguriert.
    version: '0.0.3',
    
    // WICHTIG: Power Source muss explizit definiert werden für das Battery-Icon!
    // Ohne diese Property zeigt Z2M ein "?" statt des Battery-Icons
    // Siehe: https://github.com/Koenkk/zigbee-herdsman-converters/issues/8098
    powerSource: 'Battery',
    
    // fromZigbee: Konverter für eingehende Daten vom Gerät
    fromZigbee: [
        fzLocal.gas_metering,     // Unser Custom Gas-Konverter
        fzLocal.battery_custom,   // Unser Custom Battery-Konverter
    ],
    
    // toZigbee: Konverter für ausgehende Befehle an das Gerät (hier nicht benötigt)
    toZigbee: [],
    
    // Exposes: Definiert welche Entitäten in Home Assistant erscheinen
    exposes: [
        // Batteriestand in % (0-100) - verwendet Standard-Preset
        e.battery(),
        
        // Batteriespannung in V (HA device_class: voltage)
        exposes.numeric('battery_voltage', ea.STATE)
            .withUnit('V')
            .withDescription('Battery voltage [V]'),
        
        // Batterie-Warnung (boolean) - verwendet Standard-Preset
        e.battery_low(),
        
        // Gas-Zählerstand in m³ (Kubikmeter)
        exposes.numeric('gas', ea.STATE)
            .withUnit('m³')
            .withDescription('Gas consumption [m³]'),
    ],
    
    // Configure-Funktion: Wird nach dem Pairing/Join und bei „Rekonfigurieren“ ausgeführt.
    // Zuerst Basic lesen (Firmware-ID in der Z2M-UI), dann Bind + Configure Reporting.
    // Das Device setzt nur die Cluster-Werte; der Zigbee-Stack sendet Attribute Reports
    // automatisch bei Wertänderung (sobald Reporting konfiguriert ist).
    //
    // Branch zigbee_reporting_fix: Fehler nicht mehr stillschweigend mit .catch(() => {}) schlucken.
    // Pro Attribut try/catch + Ausgabe auf stdout/stderr (sichtbar in Z2M-Container-/Prozesslogs).
    //
    // Zigbee2MQTT 2.10.1 (lib/extension/configure.ts): configure(device.zh, coordinatorEndpoint, device.definition).
    // Das dritte Argument ist die Converter-Definition, kein Logger — ein Parameter "logger" waere falsch.
    configure: async (device, coordinatorEndpoint) => {
        const endpoint = device.getEndpoint(1);
        const cfgLog = {
            info: (msg) => console.log(`[gas-o-meter2 configure] ${msg}`),
            warn: (msg) => console.warn(`[gas-o-meter2 configure] ${msg}`),
        };

        // Firmware-Anzeige: endpoint.read allein aktualisiert oft nur den Cluster-Cache.
        // Die About-UI liest device.softwareBuildID — daher explizit setzen + speichern.
        // Sleepy ED: Gerät muss wach sein (sonst Timeout). Nur swBuildId (dateCode oft ungesetzt).
        try {
            const basic = await endpoint.read('genBasic', ['swBuildId']);
            const sw = basic && basic.swBuildId;
            if (sw !== undefined && sw !== null && String(sw).length > 0) {
                device.softwareBuildID = String(sw);
                if (typeof device.save === 'function') {
                    device.save();
                }
                cfgLog.info(`configure read genBasic OK: softwareBuildID=${device.softwareBuildID}`);
            } else {
                cfgLog.warn('configure read genBasic: swBuildId leer/undefined');
            }
        } catch (e) {
            cfgLog.warn(`configure read genBasic FAIL: ${e.message}`);
        }

        // Bind pro Cluster (mit Pause dazwischen): Ein kombinierter Bind fuer mehrere
        // Cluster kann bei Sleepy-EDs zu bindRsp-Timeouts fuehren (Z2M: "after 10000ms"),
        // wenn das Geraet zwischendurch wenig Luft fuer ZDO hat oder schon wieder schlaeft.
        // Reihenfolge: zuerst genPowerCfg (kleiner / oft schneller), dann seMetering.
        const bindOne = async (clusters, label) => {
            try {
                await reporting.bind(endpoint, coordinatorEndpoint, clusters);
                cfgLog.info(`configure bind OK: ${label}`);
            } catch (e) {
                cfgLog.warn(`configure bind FAIL: ${label}: ${e.message}`);
            }
        };
        await bindOne(['genPowerCfg'], 'genPowerCfg');
        await new Promise((r) => setTimeout(r, 750));
        await bindOne(['seMetering'], 'seMetering');

        const tries = [
            ['seMetering/currentSummDelivered', () =>
                reporting.report(endpoint, 'seMetering', 'currentSummDelivered',
                    {min: 300, max: 3600, change: 0})],
            ['genPowerCfg/batteryPercentageRemaining', () =>
                reporting.batteryPercentageRemaining(endpoint)],
            ['genPowerCfg/batteryVoltage', () =>
                reporting.batteryVoltage(endpoint)],
            ['genPowerCfg/batteryAlarmState', () =>
                reporting.report(endpoint, 'genPowerCfg', 'batteryAlarmState',
                    {min: 3600, max: 86400, change: 0})],
        ];
        for (const [name, fn] of tries) {
            try {
                await fn();
                cfgLog.info(`configure reporting OK: ${name}`);
            } catch (e) {
                cfgLog.warn(`configure reporting FAIL: ${name}: ${e.message}`);
            }
        }
    },

    meta: {
        battery: {type: 'battery'},  // Sagt Z2M: "Dieses Device ist batteriebetrieben" → zeigt Batterie-Icon statt "?"
        // configureKey erhoehen: Z2M ruft configure() erneut auf, sobald sich der Key aendert (ohne Re-Pair).
        configureKey: 6,
    },
};

// Exportiere die Definition für Zigbee2MQTT
module.exports = definition;
