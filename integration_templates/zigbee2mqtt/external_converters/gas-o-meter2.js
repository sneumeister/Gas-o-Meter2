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
            
            // Battery Voltage (Attribut 0x0020) als battery_voltage
            // Zigbee sendet uint8 in 100mV Einheiten (z.B. 35 = 3.5V)
            // Z2M erwartet mV (wie gewünscht)
            if (msg.data.hasOwnProperty('batteryVoltage')) {
                result.battery_voltage = msg.data['batteryVoltage'] * 100; // 100mV -> mV
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
        
        // Batteriespannung in mV - Custom Expose mit battery_voltage
        exposes.numeric('battery_voltage', ea.STATE)
            .withUnit('mV')
            .withDescription('Battery voltage [mV]'),
        
        // Batterie-Warnung (boolean) - verwendet Standard-Preset
        e.battery_low(),
        
        // Gas-Zählerstand in m³ (Kubikmeter)
        exposes.numeric('gas', ea.STATE)
            .withUnit('m³')
            .withDescription('Gas consumption [m³]'),
    ],
    
    // Configure-Funktion: Wird nach dem Pairing/Join ausgeführt
    // Nur Bind – kein configureReporting. Das Device sendet ausschließlich manuelle
    // Attribute Reports (esp_zb_zcl_report_attr_cmd_req); automatisches Reporting wird
    // in der Firmware mit esp_zb_zcl_stop_attr_reporting deaktiviert. So vermeiden wir
    // Doppel-Reports und wechselnde Spannungswerte (z. B. 4.0 V vs 4.2 V) in Z2M/HA.
    configure: async (device, coordinatorEndpoint) => {
        const endpoint = device.getEndpoint(1);
        await reporting.bind(endpoint, coordinatorEndpoint, ['seMetering', 'genPowerCfg']);
    },
    
    meta: {
        battery: {type: 'battery'},  // Sagt Z2M: "Dieses Device ist batteriebetrieben" → zeigt Batterie-Icon statt "?"
        configureKey: 1,  // Stellt sicher, dass die Konfiguration bei jedem Join/Reboot versucht wird
    },
};

// Exportiere die Definition für Zigbee2MQTT
module.exports = definition;
