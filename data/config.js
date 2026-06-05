// WiFi-Credentials dynamisch verwalten
let wifiCredentials = [];

/**
 * Ziel-URL nach Reboot: immer mDNS (STA: direkt; AP: Captive Portal leitet auf 10.0.0.1 um).
 */
function getMdnsHomeUrl(hostname) {
    const h = (hostname || '').trim().toLowerCase();
    if (!h) {
        return null;
    }
    return 'http://' + h + '.local/';
}

function getHostnameFromForm() {
    const hostnameInput = document.getElementById('hostname');
    return hostnameInput ? hostnameInput.value.trim() : '';
}

// Reboot-Funktion - vereinfacht: Countdown, dann Link anzeigen
function rebootDevice() {
    // Sicherheitsfrage
    if (!confirm("Möchten Sie das Gerät wirklich neu starten?")) {
        return;
    }
    
    const formData = new FormData();
    formData.append('cmd', 'reboot');
    
    fetch('/reboot', {
        method: 'POST',
        body: formData
    })
    .then(response => {
        if (!response.ok) {
            // Fehler-Response (z.B. 409 Conflict)
            return response.json().then(data => {
                throw new Error(data.error || `HTTP ${response.status}`);
            });
        }
        console.log("Reboot-Request erfolgreich gesendet");
    })
    .catch(error => {
        // Fehler anzeigen (z.B. Factory-Reset läuft)
        alert("Reboot nicht möglich:\n\n" + error.message);
        
        // Button wieder aktivieren
        const rebootButton = document.querySelector('button[onclick="rebootDevice()"]');
        if (rebootButton) {
            rebootButton.disabled = false;
            rebootButton.textContent = 'Reboot';
        }
        
        // Countdown-Intervall beenden (falls aktiv)
        if (typeof countdownInterval !== 'undefined') {
            clearInterval(countdownInterval);
        }
        
        console.error("Reboot-Fehler:", error);
    });
    
    // Countdown: 5 Sekunden
    let waitTime = 5;
    const rebootButton = document.querySelector('button[onclick="rebootDevice()"]');
    
    const countdownInterval = setInterval(() => {
        if (waitTime > 0) {
            if (rebootButton) {
                rebootButton.textContent = `Reboot wird durchgeführt... ${waitTime}s`;
            }
            waitTime--;
        } else {
            clearInterval(countdownInterval);
            
            // Nach Countdown: UI ändern - Formular ausblenden, Link anzeigen
            showRebootLink();
        }
    }, 1000);
}

function showRebootLink() {
    // 1. Erfolgs-Fenster entfernen (falls vorhanden)
    const successAlerts = document.querySelectorAll('.alert-success');
    successAlerts.forEach(alert => alert.remove());
    
    // 2. Formular ausblenden
    const configForm = document.getElementById('configForm');
    if (configForm) {
        configForm.style.display = 'none';
    }
    
    // 3. Erstelle neuen Inhalt mit Link
    const linkContainer = document.createElement('div');
    linkContainer.className = 'text-center';
    linkContainer.style.cssText = 'padding: 40px 20px;';
    
    const infoText = document.createElement('p');
    infoText.className = 'mb-4';
    infoText.style.cssText = 'font-size: 1.1em; color: #333;';
    const homeUrl = getMdnsHomeUrl(getHostnameFromForm());
    if (homeUrl) {
        infoText.textContent = 'Nach dem Neustart immer diese Adresse öffnen (WLAN: mDNS; Konfig-AP: Captive Portal leitet um). Warten Sie, bis das Gerät online ist:';
    } else {
        infoText.textContent = 'Warten Sie, bis das System wieder online ist (Hostname im Formular fehlt):';
    }
    
    const linkElement = document.createElement('a');
    linkElement.className = 'btn btn-primary btn-lg';
    linkElement.style.cssText = 'font-size: 1.2em; padding: 15px 30px; text-decoration: none; display: inline-block; margin-top: 20px;';
    if (homeUrl) {
        linkElement.href = homeUrl;
        linkElement.textContent = homeUrl;
    } else {
        linkElement.href = '/';
        linkElement.textContent = window.location.origin + '/';
    }
    
    linkContainer.appendChild(infoText);
    linkContainer.appendChild(document.createElement('br'));
    linkContainer.appendChild(linkElement);
    
    // Füge neuen Inhalt nach der Überschrift ein
    const configContainer = document.querySelector('.config-container');
    if (configContainer) {
        // Füge nach der Überschrift ein
        const heading = configContainer.querySelector('h1');
        if (heading && heading.nextSibling) {
            configContainer.insertBefore(linkContainer, heading.nextSibling);
        } else {
            configContainer.appendChild(linkContainer);
        }
    }
}

// Stay-Alive-Mechanismus für Config-Seite
let stayAliveInterval = null;
let originalHostname = null;  // Ursprünglicher Hostname beim Laden der Seite

function syncTxPowerSelects() {
    const wifiCurrent = document.getElementById('wifi_tx_power_dbm_current');
    const wifiSelect = document.getElementById('wifi_tx_power_dbm');
    if (wifiCurrent && wifiSelect) {
        wifiSelect.value = wifiCurrent.value;
    }

    const bleCurrent = document.getElementById('ble_tx_power_dbm_current');
    const bleSelect = document.getElementById('ble_tx_power_dbm');
    if (bleCurrent && bleSelect) {
        bleSelect.value = bleCurrent.value;
    }

    const zigbeeCurrent = document.getElementById('zigbee_tx_power_dbm_current');
    const zigbeeSelect = document.getElementById('zigbee_tx_power_dbm');
    if (zigbeeCurrent && zigbeeSelect) {
        zigbeeSelect.value = zigbeeCurrent.value;
    }
}

// Beim Laden: Aktuelle Config laden und Event-Listener registrieren
window.addEventListener('load', () => {
    loadWifiCredentials();
    syncTxPowerSelects();
    
    // Ursprünglichen Hostname speichern (für Stay-Alive und Config-Save)
    const hostnameInput = document.getElementById('hostname');
    if (hostnameInput) {
        originalHostname = hostnameInput.getAttribute('value') || 
                          hostnameInput.defaultValue || 
                          hostnameInput.value;
    }
    
    // Event-Listener für Admin-Passwort-Validierung
    const adminpassNew = document.getElementById('adminpass_new');
    const adminpassConfirm = document.getElementById('adminpass_confirm');
    
    if (adminpassNew && adminpassConfirm) {
        adminpassNew.addEventListener('input', checkAdminPassMatch);
        adminpassConfirm.addEventListener('input', checkAdminPassMatch);
    }
    
    // Stay-Alive-Mechanismus starten: Regelmäßige Ping-Anfragen, um ESP32 wach zu halten
    // Intervall: 2 Minuten (weniger als WIFI_WAIT_FOR_SLEEP = 3 Minuten)
    startStayAlive();
});

// Stay-Alive beim Verlassen der Seite stoppen
window.addEventListener('beforeunload', () => {
    stopStayAlive();
});

// Stay-Alive starten
function startStayAlive() {
    // Bereits laufendes Interval stoppen, falls vorhanden
    stopStayAlive();
    
    // Ping alle 2 Minuten (120 Sekunden) - weniger als WIFI_WAIT_FOR_SLEEP (3 Minuten)
    stayAliveInterval = setInterval(() => {
        fetch('/ping', {
            method: 'GET',
            cache: 'no-cache'
        })
        .then(response => {
            if (response.ok) {
                console.log('Stay-Alive: ESP32 ist wach');
            }
        })
        .catch(error => {
            console.log('Stay-Alive: Fehler (ESP32 möglicherweise im Deep-Sleep):', error.message);
        });
    }, 120000); // 2 Minuten = 120000 ms
}

// Stay-Alive stoppen
function stopStayAlive() {
    if (stayAliveInterval) {
        clearInterval(stayAliveInterval);
        stayAliveInterval = null;
    }
}

// WLAN-Scan-Bereich aufklappen/zuklappen
function toggleWifiScan() {
    const collapseDiv = document.getElementById('wifiScanCollapse');
    const toggleBtn = document.getElementById('wifiScanToggle');
    
    if (collapseDiv.style.display === 'none') {
        // Aufklappen
        collapseDiv.style.display = 'block';
        toggleBtn.textContent = '📡 WLAN in der Nähe... (ausblenden)';
        // Netzwerke laden, wenn noch nicht geladen
        if (document.getElementById('wifiNetworksList').innerHTML === '') {
            loadWifiNetworks();
        }
    } else {
        // Zuklappen
        collapseDiv.style.display = 'none';
        toggleBtn.textContent = '📡 WLAN in der Nähe...';
    }
}

// WLAN-Netzwerke laden und anzeigen
function loadWifiNetworks() {
    const statusDiv = document.getElementById('wifiScanStatus');
    const listDiv = document.getElementById('wifiNetworksList');
    
    statusDiv.textContent = 'Lade verfügbare Netzwerke...';
    listDiv.innerHTML = '';
    
    // Aktuelles Admin-Passwort für Basic-Auth
    const currentAdminPass = document.getElementById('adminpass').value;
    const authHeader = 'Basic ' + btoa('admin:' + currentAdminPass);
    
    fetch('/wifi/scan', {
        method: 'GET',
        headers: {
            'Authorization': authHeader
        }
    })
    .then(response => {
        if (!response.ok) {
            if (response.status === 401) {
                throw new Error('Authentifizierung fehlgeschlagen. Bitte Seite neu laden.');
            }
            throw new Error('Fehler beim Laden der Netzwerke');
        }
        return response.json();
    })
    .then(networks => {
        statusDiv.textContent = '';
        
        if (networks.length === 0) {
            listDiv.innerHTML = '<p class="text-muted">Keine Netzwerke gefunden.</p>';
            return;
        }
        
        // Tabelle erstellen
        let tableHTML = '<table class="table table-sm table-striped"><thead><tr><th>SSID</th><th>RSSI</th><th>Authentifizierung</th></tr></thead><tbody>';
        
        networks.forEach(network => {
            const encryptedIcon = network.encrypted ? '🔒' : '🔓';
            const encryptedText = network.encrypted ? 'Erforderlich' : 'Offen';
            const rssiClass = network.rssi > -50 ? 'text-success' : (network.rssi > -70 ? 'text-warning' : 'text-danger');
            
            tableHTML += `<tr>
                <td><strong>${escapeHtml(network.ssid)}</strong></td>
                <td class="${rssiClass}">${network.rssi} dBm</td>
                <td>${encryptedIcon} ${encryptedText}</td>
            </tr>`;
        });
        
        tableHTML += '</tbody></table>';
        if (networks.length >= 10) {
            tableHTML += '<p class="small text-muted mt-2 mb-0">Liste begrenzt auf 10 Einträge — bei vielen WLANs in der Nähe fehlen eventuell schwächere Netze.</p>';
        }
        listDiv.innerHTML = tableHTML;
    })
    .catch(error => {
        statusDiv.textContent = '';
        listDiv.innerHTML = `<p class="text-danger">Fehler: ${error.message}</p>`;
    });
}

// HTML-Escape-Funktion (verhindert XSS)
function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

// Passwort-Feld zwischen Klartext und versteckt umschalten (iconId = Span mit Augen-Icon)
function togglePassword(inputId, iconId) {
    const input = document.getElementById(inputId);
    const icon = document.getElementById(iconId);
    if (!input || !icon) {
        return;
    }
    if (input.type === 'password') {
        input.type = 'text';
        icon.textContent = '🙈';
    } else {
        input.type = 'password';
        icon.textContent = '👁️';
    }
}

// Admin-Passwort-Feld freischalten
function unlockAdminPass() {
    document.getElementById('adminpassLocked').style.display = 'none';
    document.getElementById('adminpassUnlocked').style.display = 'block';
    document.getElementById('adminpass_new').focus();
}

// Admin-Passwort-Feld sperren (Änderung abbrechen)
function lockAdminPass() {
    document.getElementById('adminpassLocked').style.display = 'block';
    document.getElementById('adminpassUnlocked').style.display = 'none';
    // Felder zurücksetzen
    document.getElementById('adminpass_new').value = '';
    document.getElementById('adminpass_confirm').value = '';
    document.getElementById('adminpassMatch').style.display = 'none';
    document.getElementById('adminpassMatchOk').style.display = 'none';
    // Input-Validierung zurücksetzen
    document.getElementById('adminpass_new').classList.remove('is-invalid', 'is-valid');
    document.getElementById('adminpass_confirm').classList.remove('is-invalid', 'is-valid');
}

// Passwort-Übereinstimmung prüfen
function checkAdminPassMatch() {
    const newPass = document.getElementById('adminpass_new').value;
    const confirmPass = document.getElementById('adminpass_confirm').value;
    const matchDiv = document.getElementById('adminpassMatch');
    const matchOkDiv = document.getElementById('adminpassMatchOk');
    const newInput = document.getElementById('adminpass_new');
    const confirmInput = document.getElementById('adminpass_confirm');
    
    if (confirmPass.length === 0) {
        // Noch keine Eingabe
        matchDiv.style.display = 'none';
        matchOkDiv.style.display = 'none';
        newInput.classList.remove('is-invalid', 'is-valid');
        confirmInput.classList.remove('is-invalid', 'is-valid');
        return false;
    }
    
    if (newPass === confirmPass && newPass.length > 0) {
        // Passwörter stimmen überein
        matchDiv.style.display = 'none';
        matchOkDiv.style.display = 'block';
        newInput.classList.remove('is-invalid');
        newInput.classList.add('is-valid');
        confirmInput.classList.remove('is-invalid');
        confirmInput.classList.add('is-valid');
        return true;
    } else {
        // Passwörter stimmen nicht überein
        matchDiv.style.display = 'block';
        matchOkDiv.style.display = 'none';
        newInput.classList.remove('is-valid');
        newInput.classList.add('is-invalid');
        confirmInput.classList.remove('is-valid');
        confirmInput.classList.add('is-invalid');
        return false;
    }
}


function loadWifiCredentials() {
    // Lade aktuelle WiFi-Credentials aus der Seite (werden vom Server eingefügt)
    wifiCredentials = [];
    
    // Prüfe, ob WiFi-Credentials vorhanden sind (vom Server als JSON eingefügt)
    const wifiDataElement = document.getElementById('wifiCredentialsData');
    if (wifiDataElement) {
        try {
            const jsonText = wifiDataElement.textContent.trim();
            if (jsonText && jsonText !== '[]') {
                wifiCredentials = JSON.parse(jsonText);
            }
        } catch (e) {
            console.error("Fehler beim Parsen der WiFi-Credentials:", e);
        }
    }
    
    // Wenn keine Credentials vorhanden, ein leeres Set anzeigen
    if (wifiCredentials.length === 0) {
        wifiCredentials = [{ssid: "", password: ""}];
    }
    
    renderWifiCredentials();
}

function renderWifiCredentials() {
    const container = document.getElementById('wifiCredentials');
    container.innerHTML = '';
    
    wifiCredentials.forEach((cred, index) => {
        const setDiv = document.createElement('div');
        setDiv.className = 'card mb-3';
        setDiv.innerHTML = `
            <div class="card-body">
                <h5 class="card-title h6 mb-3" style="color: #667eea;">WiFi-Set ${index + 1}</h5>
                <div class="mb-3">
                    <label for="wifi_ssid_${index}" class="form-label">SSID</label>
                    <input type="text" class="form-control" id="wifi_ssid_${index}" name="wifi_ssid_${index}" value="${cred.ssid || ''}" required>
                </div>
                <div class="mb-3">
                    <label for="wifi_password_${index}" class="form-label">Passwort</label>
                    <div class="input-group">
                        <input type="password" class="form-control" id="wifi_password_${index}" name="wifi_password_${index}" value="${cred.password || ''}" required>
                        <button class="btn btn-outline-secondary" type="button" id="toggleWifiPass_${index}" onclick="togglePassword('wifi_password_${index}', 'toggleWifiPass_${index}Icon')">
                            <span id="toggleWifiPass_${index}Icon">👁️</span>
                        </button>
                    </div>
                </div>
                ${index > 0 ? `<button type="button" class="btn btn-danger btn-sm" onclick="removeWifiCredential(${index})">- Entfernen</button>` : ''}
            </div>
        `;
        container.appendChild(setDiv);
    });
    
    // + Button anzeigen, wenn weniger als 2 Sets vorhanden
    const addBtn = document.getElementById('addWifiBtn');
    if (wifiCredentials.length < 2) {
        addBtn.style.display = 'inline-block';
    } else {
        addBtn.style.display = 'none';
    }
}

function addWifiCredential() {
    if (wifiCredentials.length < 2) {
        wifiCredentials.push({ssid: "", password: ""});
        renderWifiCredentials();
    }
}

function removeWifiCredential(index) {
    if (wifiCredentials.length > 1 && index > 0) {
        wifiCredentials.splice(index, 1);
        renderWifiCredentials();
    }
}

function reloadConfig() {
    if (confirm("Möchten Sie die Konfiguration wirklich neu laden? Alle nicht gespeicherten Änderungen gehen verloren.")) {
        // Seite neu laden - Cache-Control-Header sollten ausreichen
        // Die Config wird automatisch aus RTC-RAM geladen
        window.location.reload(true);  // true = Hard Reload (ignoriert Cache)
    }
}

function saveConfig() {
    // Prüfe, ob Admin-Passwort geändert werden soll
    const adminpassNew = document.getElementById('adminpass_new');
    const adminpassConfirm = document.getElementById('adminpass_confirm');
    const adminpassUnlocked = document.getElementById('adminpassUnlocked');
    
    let adminpass = document.getElementById('adminpass').value; // Aktuelles Passwort
    
    // Wenn Passwort-Felder sichtbar sind, wurde versucht, das Passwort zu ändern
    if (adminpassUnlocked && adminpassUnlocked.style.display !== 'none') {
        const newPass = adminpassNew.value.trim();
        const confirmPass = adminpassConfirm.value.trim();
        
        if (newPass.length > 0 || confirmPass.length > 0) {
            // Es wurde versucht, das Passwort zu ändern
            if (newPass.length === 0) {
                alert("Bitte geben Sie ein neues Admin-Passwort ein.");
                return;
            }
            
            if (confirmPass.length === 0) {
                alert("Bitte wiederholen Sie das neue Admin-Passwort.");
                return;
            }
            
            if (newPass !== confirmPass) {
                alert("Die Passwörter stimmen nicht überein. Bitte korrigieren Sie die Eingabe.");
                checkAdminPassMatch(); // Visuelles Feedback aktualisieren
                return;
            }
            
            // Neues Passwort verwenden
            adminpass = newPass;
        }
    }
    
    // Sammle alle Formular-Daten mit Validierung
    const hostname = document.getElementById('hostname').value.trim();
    const wakeupMinutesStr = document.getElementById('wakeup_minutes').value.trim();
    const transferMode = document.getElementById('transfer_mode').value.trim();
    const transferMinutesStr = document.getElementById('transfer_minutes').value.trim();
    const adcMultiplierStr = document.getElementById('adc_voltage_multiplier').value.trim();
    const ntpServer = document.getElementById('ntp_server').value.trim();
    const mqttHostInput = document.getElementById('mqtt_host');
    const mqttPortInput = document.getElementById('mqtt_port');
    const mqttUsernameInput = document.getElementById('mqtt_username');
    const mqttPasswordInput = document.getElementById('mqtt_password');
    const mqttMainTopicInput = document.getElementById('mqtt_main_topic');
    const mqttHaCheckbox = document.getElementById('mqtt_ha_autodiscovery');
    const mqttDummyHostDefaultInput = document.getElementById('mqtt_dummy_host_default');
    const mqttDefaultPortInput = document.getElementById('mqtt_default_port_value');
    const mqttDefaultMainTopicInput = document.getElementById('mqtt_default_main_topic_value');
    const mqttHost = mqttHostInput ? mqttHostInput.value.trim() : '';
    const mqttPortStr = mqttPortInput ? mqttPortInput.value.trim() : '';
    const mqttUsername = mqttUsernameInput ? mqttUsernameInput.value.trim() : '';
    const mqttPassword = mqttPasswordInput ? mqttPasswordInput.value.trim() : '';
    const mqttMainTopic = mqttMainTopicInput ? mqttMainTopicInput.value.trim() : '';
    const mqttHaAutodiscovery = mqttHaCheckbox ? !!mqttHaCheckbox.checked : false;
    const mqttDummyHostDefault = (mqttDummyHostDefaultInput && mqttDummyHostDefaultInput.value.trim().length > 0)
        ? mqttDummyHostDefaultInput.value.trim()
        : 'dummy_mqtt_host';
    const mqttDefaultPort = (mqttDefaultPortInput && mqttDefaultPortInput.value.trim().length > 0)
        ? parseInt(mqttDefaultPortInput.value.trim(), 10)
        : 1883;
    const mqttDefaultMainTopic = (mqttDefaultMainTopicInput && mqttDefaultMainTopicInput.value.trim().length > 0)
        ? mqttDefaultMainTopicInput.value.trim()
        : hostname;

    const wifiTxPowerDbm = parseInt(document.getElementById('wifi_tx_power_dbm').value);
    const bleTxPowerDbm = parseInt(document.getElementById('ble_tx_power_dbm').value);
    const zigbeeTxPowerDbm = parseInt(document.getElementById('zigbee_tx_power_dbm').value);
    
    // Validierung: Hostname (max. 26 Zeichen, HOSTNAME_MAX_LEN / BLE-Stack)
    if (hostname.length === 0 || hostname.length > 26) {
        alert("Hostname muss 1-26 Zeichen lang sein (Limit für BLE, mDNS und MQTT/HA).");
        document.getElementById('hostname').focus();
        return;
    }
    
    // Validierung: Wake-up Intervall
    const wakeup_minutes = parseInt(wakeupMinutesStr);
    if (isNaN(wakeup_minutes) || wakeup_minutes < 1 || wakeup_minutes > 60) {
        alert("Bitte geben Sie ein gültiges Wake-up Intervall zwischen 1 und 60 Minuten an.");
        document.getElementById('wakeup_minutes').focus();
        return;
    }
    
    // Validierung: Transfer-Mode
    if (transferMode.length === 0 || 
        (transferMode !== 'none' && transferMode !== 'zigbee' && transferMode !== 'ble' && transferMode !== 'mqtt')) {
        alert("Bitte wählen Sie einen gültigen Transfer-Mode aus.");
        document.getElementById('transfer_mode').focus();
        return;
    }
    
    // Validierung: Transfer Intervall (Multiplikator)
    const transfer_minutes = parseInt(transferMinutesStr);
    if (isNaN(transfer_minutes) || transfer_minutes < 0 || transfer_minutes > 60) {
        alert("Bitte geben Sie einen gültigen Transfer Intervall Multiplikator zwischen 0 und 60 an (0 = nie).");
        document.getElementById('transfer_minutes').focus();
        return;
    }
    // Prüfen, ob Multiplikator * Wake-up-Intervall <= 60
    if (transfer_minutes > 0) {
        const wakeup_minutes = parseInt(wakeupMinutesStr);
        const calculated_minutes = transfer_minutes * wakeup_minutes;
        if (calculated_minutes > 60) {
            alert(`Transfer Intervall ungültig: ${transfer_minutes} * ${wakeup_minutes} = ${calculated_minutes} Min. (muss <= 60 sein)`);
            document.getElementById('transfer_minutes').focus();
            return;
        }
    }
    
    // Validierung: ADC Spannungs-Multiplikator
    const adc_voltage_multiplier = parseFloat(adcMultiplierStr);
    if (isNaN(adc_voltage_multiplier) || adc_voltage_multiplier < 0.5 || adc_voltage_multiplier > 2.0) {
        alert("Bitte geben Sie einen gültigen ADC Spannungs-Multiplikator zwischen 0.5 und 2.0 ein.");
        document.getElementById('adc_voltage_multiplier').focus();
        return;
    }

    // Validierung: TX-Power (nur erlaubte UI-Stufen)
    const wifiAllowed = [2, 5, 8, 11, 14, 17, 20];
    if (isNaN(wifiTxPowerDbm) || !wifiAllowed.includes(wifiTxPowerDbm)) {
        alert("WiFi TX Power ungültig (erlaubt: 2,5,8,11,14,17,20 dBm).");
        document.getElementById('wifi_tx_power_dbm').focus();
        return;
    }

    const bleAllowed = [3, 6, 9, 12, 15, 18, 20];
    if (isNaN(bleTxPowerDbm) || !bleAllowed.includes(bleTxPowerDbm)) {
        alert("BLE TX Power ungültig (erlaubt: 3,6,9,12,15,18,20 dBm).");
        document.getElementById('ble_tx_power_dbm').focus();
        return;
    }

    const zigbeeAllowed = [-9, -6, -3, 0, 3, 6, 10];
    if (isNaN(zigbeeTxPowerDbm) || !zigbeeAllowed.includes(zigbeeTxPowerDbm)) {
        alert("ZigBee TX Power ungültig (erlaubt: -9,-6,-3,0,3,6,10 dBm).");
        document.getElementById('zigbee_tx_power_dbm').focus();
        return;
    }
    
    // Validierung: NTP-Server
    if (ntpServer.length === 0) {
        alert("Bitte geben Sie einen NTP-Server an.");
        document.getElementById('ntp_server').focus();
        return;
    }
    
    // Validierung: Admin-Passwort
    if (adminpass.length === 0) {
        alert("Bitte geben Sie ein Admin-Passwort an.");
        return;
    }

    // MQTT-spezifische Validierung
    let mqttPort = mqttDefaultPort;
    let mqttHostForSave = mqttHost;
    if (transferMode === 'mqtt') {
        if (mqttHost.length === 0) {
            mqttHostForSave = mqttDummyHostDefault;
        }

        mqttPort = (mqttPortStr.length === 0) ? mqttDefaultPort : parseInt(mqttPortStr, 10);
        if (isNaN(mqttPort) || mqttPort < 1 || mqttPort > 65535) {
            alert("Bitte geben Sie einen gültigen MQTT Port zwischen 1 und 65535 an.");
            document.getElementById('mqtt_port').focus();
            return;
        }
    }
    
    const formData = {
        hostname: hostname,
        adminpass: adminpass,
        wakeup_minutes: wakeup_minutes,
        transfer_mode: transferMode,
        transfer_minutes: transfer_minutes,
        adc_voltage_multiplier: adc_voltage_multiplier,
        ntp_server: ntpServer,
        mqtt_host: mqttHostForSave,
        mqtt_port: mqttPort,
        mqtt_username: mqttUsername,
        mqtt_password: mqttPassword,
        mqtt_main_topic: mqttMainTopic.length > 0 ? mqttMainTopic : mqttDefaultMainTopic,
        mqtt_ha_autodiscovery: mqttHaAutodiscovery,
        wifi_tx_power_dbm: wifiTxPowerDbm,
        ble_tx_power_dbm: bleTxPowerDbm,
        zigbee_tx_power_dbm: zigbeeTxPowerDbm,
        wifiCredentials: []
    };
    
    // Sammle WiFi-Credentials aus den gerenderten Feldern
    // Verwende die Anzahl der WiFi-Sets aus dem wifiCredentials Array
    for (let i = 0; i < wifiCredentials.length; i++) {
        const ssidInput = document.getElementById(`wifi_ssid_${i}`);
        const passwordInput = document.getElementById(`wifi_password_${i}`);
        
        // Prüfe, ob die Input-Felder existieren (könnten entfernt worden sein)
        if (ssidInput && passwordInput) {
            const ssid = ssidInput.value.trim();
            const password = passwordInput.value.trim();
            
            // Verwende eingegebene Werte, oder falls leer, die ursprünglichen Werte
            const finalSsid = ssid.length > 0 ? ssid : (wifiCredentials[i].ssid || '');
            const finalPassword = password.length > 0 ? password : (wifiCredentials[i].password || '');
            
            // Nur nicht-leere Sets hinzufügen (mindestens SSID muss vorhanden sein)
            if (finalSsid.length > 0) {
                formData.wifiCredentials.push({
                    ssid: finalSsid,
                    password: finalPassword  // Passwort kann leer sein (offenes Netzwerk)
                });
            }
        }
    }
    
    // Prüfe, ob mindestens ein WiFi-Set vorhanden ist
    if (formData.wifiCredentials.length === 0) {
        alert("Bitte geben Sie mindestens ein WiFi-Set (SSID) an.");
        return;
    }
    
    // 2-stufige Sicherheitsabfrage
    // Vergleiche die ursprünglich konfigurierten WiFi-Credentials mit den neuen
    const originalWifi = getOriginalWifiCredentials();
    const wifiChanged = hasWifiChanged(originalWifi, formData.wifiCredentials);
    
    // Zusätzlich prüfen, ob das aktuell verbundene Netzwerk noch vorhanden ist
    const currentWifi = getCurrentWifiCredentials();
    const currentWifiStillAvailable = currentWifi.length > 0 && 
        formData.wifiCredentials.some(cred => 
            cred.ssid === currentWifi[0].ssid && cred.password === currentWifi[0].password
        );
    
    if (wifiChanged && !currentWifiStillAvailable) {
        if (!confirm("WARNUNG: Die WiFi-Credentials unterscheiden sich vom aktuell verbundenen Netzwerk!\n\n" +
                    "Möglicherweise können Sie nach dem Speichern keine Verbindung mehr herstellen.\n\n" +
                    "Möchten Sie trotzdem fortfahren?")) {
            return;
        }
    }
    
    if (!confirm("Möchten Sie wirklich alle Konfigurationsdaten speichern?\n\n" +
                "Das Gerät wird die neue Konfiguration übernehmen und neu starten.")) {
        return;
    }
    
    // Prüfe, was geändert wurde (für intelligente Reload-Strategie)
    // originalWifi und wifiChanged wurden bereits oben berechnet (Zeile 393-394)
    const originalHostname = document.getElementById('hostname').getAttribute('value') || 
                             document.getElementById('hostname').defaultValue || 
                             document.getElementById('hostname').value;
    const newHostname = formData.hostname;
    const hostnameChanged = (originalHostname !== newHostname);
    
    // Sende Daten an Server
    const formDataToSend = new FormData();
    formDataToSend.append('data', JSON.stringify(formData));
    
    // Aktuelles Passwort (aus dem gesperrten Feld) als zusätzliche Sicherheit mitsenden
    // Dies verhindert Config-Injection ohne Admin-Passwort und löst das Deadlock-Problem:
    // - Wenn neues Passwort gespeichert wird, wird es sofort aktiv
    // - Basic-Auth verwendet noch das alte Passwort
    // - current_password Parameter ermöglicht die Authentifizierung mit dem alten Passwort
    const currentAdminPass = document.getElementById('adminpass').value;
    formDataToSend.append('current_password', currentAdminPass);
    
    // Basic-Auth-Credentials aus dem aktuellen Passwort-Feld (für Fallback)
    const authHeader = 'Basic ' + btoa('admin:' + currentAdminPass);
    
    const savePath = '/config/save';
    
    console.log('Config-Save: Pfad =', savePath);
    console.log('Config-Save: Hostname geändert =', hostnameChanged);
    console.log('Config-Save: Original Hostname =', originalHostname);
    console.log('Config-Save: Neuer Hostname =', newHostname);
    console.log('Config-Save: window.location.hostname =', window.location.hostname);
    
    fetch(savePath, {
        method: 'POST',
        headers: {
            'Authorization': authHeader
        },
        body: formDataToSend
    })
    .then(response => {
        // WICHTIG: Warte auf vollständige Antwort (response.ok prüft nur Status, nicht ob Daten vollständig sind)
        if (!response.ok) {
            // 4xx-Fehler: Server hat Fehler zurückgegeben
            return response.text().then(text => {
                throw new Error(text || `HTTP ${response.status}`);
            });
        }
        
        // 200-OK: Parse JSON-Antwort
        return response.json();
    })
    .then(data => {
        // JSON-Antwort erfolgreich empfangen
        if (data.success) {
            // Zeige Erfolgsmeldung mit empfangenen JSON-Daten (für Debugging)
            const successAlert = document.createElement('div');
            successAlert.className = 'alert alert-success alert-dismissible fade show';
            successAlert.setAttribute('role', 'alert');
            successAlert.style.cssText = 'margin-bottom: 20px;';
            
            let alertMessage = `<strong>✓ ${data.message}</strong><br>`;
            alertMessage += `Die Änderungen wurden in config.json geschrieben.<br>`;
            if (data.wifi_changed) {
                alertMessage += `<strong style="color: #dc3545;">⚠️ WiFi-Credentials wurden geändert!</strong><br>`;
            }
            alertMessage += `<strong>Bitte betätigen Sie den "Reboot"-Button oder warten Sie bis zum nächsten Deep-Sleep-Wake-up, damit die Änderungen wirksam werden.</strong><br><br>`;
            
            // Zeige empfangene JSON-Daten (für Debugging)
            alertMessage += `<details style="margin-top: 10px;">`;
            alertMessage += `<summary style="cursor: pointer; color: #667eea;">📋 Empfangene JSON-Daten anzeigen (Debugging)</summary>`;
            alertMessage += `<pre style="background: #f8f9fa; padding: 10px; border-radius: 4px; margin-top: 10px; font-size: 0.85em; overflow-x: auto;">`;
            alertMessage += JSON.stringify(data, null, 2);
            alertMessage += `</pre>`;
            alertMessage += `</details>`;
            
            alertMessage += `<button type="button" class="btn-close" data-bs-dismiss="alert" aria-label="Close"></button>`;
            
            successAlert.innerHTML = alertMessage;
            
            // Füge Alert am Anfang des Containers ein
            const container = document.querySelector('.config-container');
            if (container) {
                container.insertBefore(successAlert, container.firstChild);
            } else {
                // Fallback: Am Anfang des Body einfügen
                document.body.insertBefore(successAlert, document.body.firstChild);
            }
            
            // Scroll zum Alert
            successAlert.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
            
            // WICHTIG: Wenn Passwort geändert wurde, aber ESP32 noch nicht neu gestartet wurde,
            // muss das adminpass Feld aktualisiert werden, damit beim nächsten Speichern
            // das richtige Passwort verwendet wird
            const adminpassNew = document.getElementById('adminpass_new');
            const adminpassUnlocked = document.getElementById('adminpassUnlocked');
            if (adminpassUnlocked && adminpassUnlocked.style.display !== 'none' && adminpassNew) {
                const newPass = adminpassNew.value.trim();
                if (newPass.length > 0) {
                    // Passwort wurde geändert: Aktualisiere das gesperrte adminpass Feld
                    // WICHTIG: Das alte Passwort bleibt im Feld, bis ESP32 neu gestartet wurde
                    // Beim nächsten Speichern wird das alte Passwort für current_password verwendet
                    // (Server erwartet noch das alte Passwort, da RTC-RAM noch nicht aktualisiert wurde)
                    // Das neue Passwort wird im adminpass Feld gespeichert (für nach dem Reboot)
                    document.getElementById('adminpass').value = newPass;
                    document.getElementById('adminpass').setAttribute('value', newPass);
                    // Passwort-Felder zurücksetzen und sperren
                    lockAdminPass();
                }
            }
            
            // WICHTIG: Wenn Hostname geändert wurde, aber noch nicht neu gestartet wurde,
            // sollte eine Warnung angezeigt werden
            if (hostnameChanged) {
                // Prüfe, ob die aktuelle URL mit dem neuen Hostname übereinstimmt
                const currentHost = window.location.hostname;
                const newHostnameLower = newHostname.toLowerCase();
                
                // Wenn die aktuelle URL nicht mit dem neuen Hostname übereinstimmt,
                // könnte der Server noch mit dem alten Hostname laufen
                // In diesem Fall: Warnung anzeigen
                if (!currentHost.includes(newHostnameLower) && !currentHost.includes('local')) {
                    // Zusätzliche Warnung hinzufügen
                    const warningDiv = document.createElement('div');
                    warningDiv.className = 'alert alert-warning mt-2';
                    warningDiv.innerHTML = `
                        <strong>⚠️ Wichtig:</strong> Der Hostname wurde geändert, aber das Gerät wurde noch nicht neu gestartet.<br>
                        Die aktuelle URL (<code>${window.location.href}</code>) stimmt möglicherweise nicht mit dem neuen Hostname überein.<br>
                        <strong>Bitte starten Sie das Gerät neu, bevor Sie weitere Änderungen vornehmen.</strong>
                    `;
                    successAlert.appendChild(warningDiv);
                }
            }

            // Gespeicherten Modus synchronisieren, damit die passende Parameter-Sektion sichtbar bleibt
            const transferModeSel = document.getElementById('transfer_mode');
            if (transferModeSel) {
                savedTransferMode = transferModeSel.value;
            }
            toggleTransferConfig();
        } else {
            throw new Error(data.message || 'Unbekannter Fehler');
        }
    })
    .catch(error => {
        // Fehler beim Speichern (4xx oder NetworkError)
        console.error("Fehler beim Speichern:", error);
        console.error("Fehler-Details:", {
            message: error.message,
            name: error.name,
            stack: error.stack,
            url: savePath,
            hostnameChanged: hostnameChanged
        });
        
        // Prüfe, ob es ein NetworkError ist und ob Hostname geändert wurde
        if (error.message.includes('NetworkError') || error.message.includes('Failed to fetch') || error.name === 'TypeError') {
            // Möglicherweise liegt es daran, dass Hostname geändert wurde, aber ESP32 noch nicht neu gestartet wurde
            if (hostnameChanged) {
                alert("Fehler beim Speichern: " + error.message + 
                      "\n\n⚠️ Mögliche Ursache: Der Hostname wurde geändert, aber das Gerät wurde noch nicht neu gestartet." +
                      "\nDie aktuelle URL (" + window.location.href + ") stimmt möglicherweise nicht mit dem neuen Hostname überein." +
                      "\n\nVersucht wurde: " + savePath +
                      "\n\nBitte starten Sie das Gerät zuerst neu, bevor Sie weitere Änderungen vornehmen.");
            } else {
                alert("Fehler beim Speichern: " + error.message + 
                      "\n\nVersucht wurde: " + savePath +
                      "\n\nBitte überprüfen Sie Ihre Eingaben und versuchen Sie es erneut.");
            }
        } else {
            alert("Fehler beim Speichern: " + error.message + 
                  "\n\nVersucht wurde: " + savePath +
                  "\n\nBitte überprüfen Sie Ihre Eingaben und versuchen Sie es erneut.");
        }
    });
}

function getOriginalWifiCredentials() {
    // Hole die ursprünglich konfigurierten WiFi-Credentials (aus wifiCredentialsData)
    // Diese wurden beim Laden der Seite in wifiCredentials gespeichert
    return wifiCredentials || [];
}

function getCurrentWifiCredentials() {
    // Versuche, aktuell verbundenes WiFi-Netzwerk aus der Seite zu extrahieren
    // (wird vom Server als verstecktes Element eingefügt)
    const currentWifiElement = document.getElementById('currentWifiData');
    if (currentWifiElement) {
        try {
            return JSON.parse(currentWifiElement.textContent);
        } catch (e) {
            console.error("Fehler beim Parsen der aktuellen WiFi-Credentials:", e);
        }
    }
    return [];
}

function hasWifiChanged(original, newCredentials) {
    // Prüfe, ob sich die WiFi-Credentials geändert haben
    // Normalisiere beide Arrays (entferne leere SSIDs)
    const originalFiltered = original.filter(cred => cred.ssid && cred.ssid.trim().length > 0);
    const newFiltered = newCredentials.filter(cred => cred.ssid && cred.ssid.trim().length > 0);
    
    if (originalFiltered.length !== newFiltered.length) {
        return true;
    }
    
    // Prüfe jedes Set: Jedes ursprüngliche Set muss in den neuen vorhanden sein
    for (let i = 0; i < originalFiltered.length; i++) {
        const found = newFiltered.find(cred => 
            cred.ssid === originalFiltered[i].ssid && cred.password === originalFiltered[i].password
        );
        if (!found) {
            return true;
        }
    }
    
    // Umgekehrt: Jedes neue Set muss auch in den ursprünglichen vorhanden sein
    for (let i = 0; i < newFiltered.length; i++) {
        const found = originalFiltered.find(cred => 
            cred.ssid === newFiltered[i].ssid && cred.password === newFiltered[i].password
        );
        if (!found) {
            return true;
        }
    }
    
    return false;
}

function showRebootProgress(hostnameChanged, wifiChanged, newHostname) {
    // Erstelle Overlay mit Countdown
    const overlay = document.createElement('div');
    overlay.id = 'rebootOverlay';
    overlay.style.cssText = `
        position: fixed;
        top: 0;
        left: 0;
        width: 100%;
        height: 100%;
        background: rgba(0, 0, 0, 0.8);
        display: flex;
        flex-direction: column;
        justify-content: center;
        align-items: center;
        z-index: 10000;
        color: white;
    `;
    
    const container = document.createElement('div');
    container.style.cssText = `
        text-align: center;
        padding: 20px;
        background: white;
        border-radius: 12px;
        color: #333;
        max-width: 350px;
        box-shadow: 0 10px 40px rgba(0, 0, 0, 0.3);
    `;
    
    const title = document.createElement('h4');
    title.textContent = 'Warten auf Reboot...';
    title.style.marginBottom = '20px';
    title.style.color = '#667eea';
    title.style.fontSize = '1.2em';
    
    // Grafischer Countdown-Ring mit Retry-Counter in der Mitte
    const countdownContainer = document.createElement('div');
    countdownContainer.style.cssText = `
        position: relative;
        width: 120px;
        height: 120px;
        margin: 0 auto 20px;
    `;
    
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('width', '120');
    svg.setAttribute('height', '120');
    svg.style.transform = 'rotate(-90deg)';
    
    // Hintergrund-Kreis
    const bgCircle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
    bgCircle.setAttribute('cx', '60');
    bgCircle.setAttribute('cy', '60');
    bgCircle.setAttribute('r', '50');
    bgCircle.setAttribute('fill', 'none');
    bgCircle.setAttribute('stroke', '#e0e0e0');
    bgCircle.setAttribute('stroke-width', '8');
    
    // Countdown-Kreis (animiert)
    const countdownCircle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
    countdownCircle.id = 'countdownCircle';
    countdownCircle.setAttribute('cx', '60');
    countdownCircle.setAttribute('cy', '60');
    countdownCircle.setAttribute('r', '50');
    countdownCircle.setAttribute('fill', 'none');
    countdownCircle.setAttribute('stroke', '#667eea');
    countdownCircle.setAttribute('stroke-width', '8');
    countdownCircle.setAttribute('stroke-linecap', 'round');
    countdownCircle.setAttribute('stroke-dasharray', '314.16'); // 2 * π * 50
    countdownCircle.setAttribute('stroke-dashoffset', '0');
    countdownCircle.style.transition = 'stroke-dashoffset 1s linear';
    
    svg.appendChild(bgCircle);
    svg.appendChild(countdownCircle);
    
    // Retry-Counter in der Mitte
    const counterText = document.createElement('div');
    counterText.id = 'retryCounter';
    counterText.style.cssText = `
        position: absolute;
        top: 50%;
        left: 50%;
        transform: translate(-50%, -50%);
        font-size: 2.5em;
        font-weight: bold;
        color: #667eea;
        line-height: 1;
    `;
    counterText.textContent = '10';
    
    countdownContainer.appendChild(svg);
    countdownContainer.appendChild(counterText);
    
    const message = document.createElement('p');
    message.id = 'rebootMessage';
    message.textContent = 'Konfiguration gespeichert. Warte auf Neustart...';
    message.style.marginBottom = '10px';
    message.style.minHeight = '40px';
    message.style.fontSize = '0.9em';
    
    const statusText = document.createElement('div');
    statusText.id = 'rebootStatus';
    statusText.style.cssText = 'font-size: 0.85em; color: #666; margin-top: 10px;';
    statusText.textContent = 'Warte auf Reboot...';
    
    container.appendChild(title);
    container.appendChild(countdownContainer);
    container.appendChild(message);
    container.appendChild(statusText);
    overlay.appendChild(container);
    document.body.appendChild(overlay);
    
    // Countdown und Reload-Logik
    let attempt = 0;
    const maxAttempts = 25; // 25 Versuche
    const attemptDelay = 5000; // 5 Sekunden zwischen Versuchen
    const initialCountdown = 10; // 10 Sekunden Countdown vor ersten Versuch
    let countdownValue = initialCountdown;
    
    // SVG-Kreis-Umfang berechnen
    const circumference = 2 * Math.PI * 50; // 314.16
    
    const updateCountdown = () => {
        // Retry-Counter aktualisieren
        counterText.textContent = countdownValue.toString();
        
        // SVG-Kreis animieren (von voll zu leer)
        const offset = circumference - (countdownValue / initialCountdown) * circumference;
        countdownCircle.setAttribute('stroke-dashoffset', offset.toString());
        
        // Status-Text aktualisieren
        if (countdownValue > 0) {
            statusText.textContent = `Warte ${countdownValue} Sekunden auf Reboot...`;
        } else {
            statusText.textContent = `Versuche Verbindung (${attempt}/${maxAttempts})...`;
        }
    };
    
    updateCountdown();
    
    // Countdown-Intervall (1 Sekunde)
    const countdownInterval = setInterval(() => {
        countdownValue--;
        if (countdownValue < 0) {
            countdownValue = 0; // Bei 0 bleiben
        }
        updateCountdown();
        
        // Wenn Countdown bei 0 angekommen ist, stoppe den Intervall
        if (countdownValue === 0) {
            clearInterval(countdownInterval);
        }
    }, 1000);
    
    const tryReload = () => {
        attempt++;
        
        // Warnung bei kritischen Änderungen
        if (attempt === 1) {
            if (wifiChanged) {
                message.innerHTML = '<strong style="color: #dc3545;">⚠️ WiFi-Credentials wurden geändert!</strong><br>Die IP-Adresse könnte sich geändert haben.';
            } else if (hostnameChanged) {
                const tryUrl = getMdnsHomeUrl(newHostname) || '/';
                message.innerHTML = '<strong style="color: #ffc107;">⚠️ Hostname wurde geändert!</strong><br>Versuche: <code>' + tryUrl + '</code>';
            } else {
                message.textContent = 'Konfiguration gespeichert. Warte auf Neustart...';
            }
        }
        
        // Status-Text aktualisieren
        statusText.textContent = `Versuche Verbindung (${attempt}/${maxAttempts})...`;
        
        // Retry-Counter für Versuche anzeigen (rückwärts von maxAttempts)
        const retriesLeft = maxAttempts - attempt + 1;
        counterText.textContent = retriesLeft.toString();
        
        // SVG-Kreis für Versuche (von voll zu leer)
        const attemptOffset = circumference - ((maxAttempts - attempt + 1) / maxAttempts) * circumference;
        countdownCircle.setAttribute('stroke-dashoffset', attemptOffset.toString());
        
        const reloadUrl = getMdnsHomeUrl(newHostname) || (window.location.origin + '/');
        
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), 5000);
        
        fetch(reloadUrl, { 
            method: 'HEAD',
            cache: 'no-cache',
            signal: controller.signal
        })
        .then(response => {
            clearTimeout(timeoutId);
            if (response.ok || response.status === 401) { // 401 = Server antwortet (Auth nötig)
                clearInterval(countdownInterval);
                message.innerHTML = '<strong style="color: #28a745;">✓ Server erreichbar!</strong><br>Lade Seite neu...';
                statusText.textContent = '';
                counterText.textContent = '✓';
                countdownCircle.setAttribute('stroke', '#28a745');
                setTimeout(() => {
                    window.location.href = reloadUrl;
                }, 500);
            } else {
                throw new Error('Server nicht erreichbar');
            }
        })
        .catch(error => {
            clearTimeout(timeoutId);
            if (attempt >= maxAttempts) {
                clearInterval(countdownInterval);
                let errorMsg = '<strong style="color: #dc3545;">⚠️ Server nicht erreichbar nach ' + maxAttempts + ' Versuchen</strong><br><br>';
                errorMsg += 'Mögliche Ursachen:<br>';
                if (wifiChanged) {
                    errorMsg += '• WiFi-Credentials wurden geändert → Neue IP-Adresse<br>';
                }
                if (hostnameChanged) {
                    errorMsg += '• Hostname wurde geändert<br>';
                }
                errorMsg += '<br>Bitte versuchen Sie:<br>';
                if (wifiChanged) {
                    errorMsg += '1. Router-Admin-Panel prüfen (neue IP-Adresse)<br>';
                }
                const homeHint = getMdnsHomeUrl(newHostname);
                if (homeHint) {
                    errorMsg += (wifiChanged ? '2' : '1') + '. Gerät öffnen: <code>' + homeHint + '</code><br>';
                    errorMsg += (wifiChanged ? '3' : '2') + '. Seite manuell neu laden';
                } else {
                    errorMsg += (wifiChanged ? '2' : '1') + '. Seite manuell neu laden';
                }
                
                message.innerHTML = errorMsg;
                statusText.textContent = '';
                counterText.textContent = '0';
                countdownCircle.setAttribute('stroke', '#dc3545');
                
                const manualButton = document.createElement('button');
                manualButton.className = 'btn btn-primary mt-3';
                manualButton.textContent = 'Seite manuell neu laden';
                manualButton.onclick = () => {
                    // Hard Reload - Cache-Control-Header sollten ausreichen
                    window.location.reload(true);  // true = Hard Reload (ignoriert Cache)
                };
                container.appendChild(manualButton);
            } else {
                // Nächster Versuch nach 5 Sekunden
                setTimeout(tryReload, attemptDelay);
            }
        });
    };
    
    // Starte erste Reload-Versuche nach 10 Sekunden (Countdown)
    setTimeout(tryReload, initialCountdown * 1000);
}

// Transfer-Konfiguration: Sektion nur für den gespeicherten Modus;
// nach Dropdown-Änderung (ohne Save) keine Parameter-Sektion.
var savedTransferMode = '';   // beim DOMContentLoaded bzw. nach Save = Dropdown-Wert (gespeicherter Modus)

function toggleTransferConfig() {
    const transferMode = document.getElementById('transfer_mode').value;
    const zigbeeSection = document.getElementById('zigbeeConfigSection');
    const bleSection = document.getElementById('bleConfigSection');
    const mqttSection = document.getElementById('mqttConfigSection');

    function hideZigbee() {
        if (zigbeeSection) zigbeeSection.style.display = 'none';
        const collapse = document.getElementById('zigbeeConfigCollapse');
        if (collapse) collapse.style.display = 'none';
    }
    function hideBle() {
        if (bleSection) bleSection.style.display = 'none';
        const collapse = document.getElementById('bleConfigCollapse');
        if (collapse) collapse.style.display = 'none';
    }
    function hideMqtt() {
        if (mqttSection) mqttSection.style.display = 'none';
        const collapse = document.getElementById('mqttConfigCollapse');
        if (collapse) collapse.style.display = 'none';
        const toggleBtn = document.getElementById('mqttConfigToggle');
        if (toggleBtn) toggleBtn.textContent = '📶 MQTT-Einstellungen...';
    }

    function ensureMqttDefaults() {
        const hostnameInput = document.getElementById('hostname');
        const mainTopicInput = document.getElementById('mqtt_main_topic');
        const portInput = document.getElementById('mqtt_port');
        const defaultPortInput = document.getElementById('mqtt_default_port_value');
        const defaultMainTopicInput = document.getElementById('mqtt_default_main_topic_value');
        const defaultMainTopic = (defaultMainTopicInput && defaultMainTopicInput.value.trim().length > 0)
            ? defaultMainTopicInput.value.trim()
            : (hostnameInput ? hostnameInput.value.trim() : '');
        const defaultPort = (defaultPortInput && defaultPortInput.value.trim().length > 0)
            ? defaultPortInput.value.trim()
            : '1883';
        if (mainTopicInput && mainTopicInput.value.trim().length === 0 && hostnameInput) {
            mainTopicInput.value = defaultMainTopic;
        }
        if (portInput && portInput.value.trim().length === 0) {
            portInput.value = defaultPort;
        }
    }

    if (transferMode === savedTransferMode) {
        if (transferMode === 'zigbee') {
            zigbeeSection.style.display = 'block';
            applyZigbeeStaOnlyState();
            hideBle();
            hideMqtt();
        } else if (transferMode === 'ble' && bleSection) {
            bleSection.style.display = 'block';
            hideZigbee();
            hideMqtt();
        } else if (transferMode === 'mqtt' && mqttSection) {
            mqttSection.style.display = 'block';
            hideZigbee();
            hideBle();
            ensureMqttDefaults();
        } else {
            hideZigbee();
            hideBle();
            hideMqtt();
        }
    } else {
        hideZigbee();
        hideBle();
        hideMqtt();
    }
}

// Rückwärtskompatibilität
function toggleZigbeeConfig() { toggleTransferConfig(); }

// MQTT-Konfiguration: Klappt das Panel auf/zu
function toggleMqttConfigPanel() {
    const collapse = document.getElementById('mqttConfigCollapse');
    const toggleBtn = document.getElementById('mqttConfigToggle');

    if (!collapse || !toggleBtn) return;

    if (collapse.style.display === 'none') {
        collapse.style.display = 'block';
        toggleBtn.textContent = '📶 MQTT-Einstellungen... (ausblenden)';
        applyMqttTestButtonState();
    } else {
        collapse.style.display = 'none';
        toggleBtn.textContent = '📶 MQTT-Einstellungen...';
    }
}

// ZigBee-Konfiguration: Klappt das Panel auf/zu
function toggleZigbeeConfigPanel() {
    const collapse = document.getElementById('zigbeeConfigCollapse');
    const toggleBtn = document.getElementById('zigbeeConfigToggle');
    
    if (collapse.style.display === 'none') {
        // Aufklappen
        collapse.style.display = 'block';
        toggleBtn.textContent = '📡 ZigBee-Einstellungen... (ausblenden)';
        // ZigBee-Status aktualisieren
        updateZigbeeStatus();
    } else {
        // Zuklappen
        collapse.style.display = 'none';
        toggleBtn.textContent = '📡 ZigBee-Einstellungen...';
    }
}

var zigbeeStatusPollTimer = null;
var zigbeeStatusPollingActive = false;
/** Inkrement pro Fetch; nur die neueste Antwort aktualisiert die Tabelle (kein Out-of-Order). */
var zigbeeStatusFetchSeq = 0;

function zigbeeJsonIsTrue(value) {
    return value === true || value === 'true';
}

// ZigBee-Status: Anzeige wie /zigbee/status (joined vor factory_new)
function zigbeeStatusLabelFromJson(data) {
    if (!data) {
        return 'Nicht aktiv';
    }
    if (zigbeeJsonIsTrue(data.joined) || data.status === 'joined') {
        return 'Gepaart';
    }
    if (zigbeeJsonIsTrue(data.pairing) || data.status === 'pairing' || data.status === 'in-progress') {
        return 'Pairing läuft…';
    }
    if (zigbeeJsonIsTrue(data.factory_new) || data.status === 'factory-new') {
        return 'Factory-New (nicht gepaart)';
    }
    return 'Nicht gepaart';
}

var ZIGBEE_STATUS_LONG_POLL_SEC = 20;
var ZIGBEE_STATUS_LONG_POLL_TIMEOUT_MS = 30000;
var ZIGBEE_STATUS_LONG_POLL_MAX_ROUNDS = 12;

function isWifiStaConnected() {
    const hidden = document.getElementById('wifi_sta_connected');
    return hidden && hidden.value === '1';
}

function applyZigbeeStaOnlyState() {
    const hint = document.getElementById('zigbeeStaHint');
    const factoryBtn = document.getElementById('zigbeeFactoryResetBtn');
    const staOk = isWifiStaConnected();
    if (hint) {
        hint.style.display = staOk ? 'none' : 'block';
    }
    if (factoryBtn) {
        factoryBtn.disabled = !staOk;
    }
    const pairingBtn = document.querySelector('button[onclick="zigbeeStartPairing()"]');
    if (pairingBtn) {
        pairingBtn.disabled = !staOk;
    }
}

function fetchZigbeeStatusJson(timeoutMs, waitSec) {
    const transferMode = document.getElementById('transfer_mode');
    if (!transferMode || transferMode.value !== 'zigbee') {
        return Promise.resolve(null);
    }
    let url = '/zigbee/status';
    if (waitSec && waitSec > 0) {
        url += '?wait=' + encodeURIComponent(String(waitSec));
    }
    const controller = new AbortController();
    const waitMs = timeoutMs || 8000;
    const timeoutId = setTimeout(function() {
        controller.abort();
    }, waitMs);
    return fetch(url, {
        method: 'GET',
        signal: controller.signal,
        cache: 'no-cache'
    })
        .finally(function() {
            clearTimeout(timeoutId);
        })
        .then(response => {
            if (!response.ok) {
                if (response.status === 400) {
                    return null;
                }
                throw new Error('Fehler beim Laden des Status');
            }
            return response.json();
        });
}

function stopZigbeeStatusPolling() {
    zigbeeStatusPollingActive = false;
    if (zigbeeStatusPollTimer !== null) {
        clearTimeout(zigbeeStatusPollTimer);
        zigbeeStatusPollTimer = null;
    }
}

function finishZigbeeStatusPolling() {
    stopZigbeeStatusPolling();
    startStayAlive();
}

function zigbeePollStatusLabel(attempts, maxAttempts, suffix) {
    let text = 'Pairing läuft… (' + attempts + '/' + maxAttempts + ')';
    if (suffix) {
        text += ' – ' + suffix;
    }
    return '<p class="zigbee-status-info">' + text + '</p>';
}

/** Nach Start Pairing: ein Long-Poll (?wait=20), Stay-Alive pausiert während offener Anfrage. */
function startZigbeeStatusLongPoll(maxRounds) {
    stopZigbeeStatusPolling();
    zigbeeStatusPollingActive = true;
    stopStayAlive();
    const statusDiv = document.getElementById('zigbeeActionStatus');
    const roundsMax = maxRounds || ZIGBEE_STATUS_LONG_POLL_MAX_ROUNDS;
    let round = 0;

    function showPollProgress(suffix) {
        if (statusDiv) {
            let text = 'Pairing läuft… (' + round + '/' + roundsMax + ')';
            if (suffix) {
                text += ' – ' + suffix;
            }
            statusDiv.innerHTML = '<p class="zigbee-status-info">' + text + '</p>';
        }
    }

    function scheduleNextPoll() {
        if (!zigbeeStatusPollingActive) {
            return;
        }
        zigbeeStatusPollTimer = setTimeout(pollOnce, 500);
    }

    function pollOnce() {
        if (!zigbeeStatusPollingActive) {
            return;
        }
        round += 1;
        showPollProgress('Long-Poll /zigbee/status?wait=' + ZIGBEE_STATUS_LONG_POLL_SEC + '…');
        zigbeeStatusFetchSeq += 1;
        const seq = zigbeeStatusFetchSeq;
        let scheduleNext = true;

        fetchZigbeeStatusJson(ZIGBEE_STATUS_LONG_POLL_TIMEOUT_MS, ZIGBEE_STATUS_LONG_POLL_SEC)
            .then(function(data) {
                if (seq !== zigbeeStatusFetchSeq) {
                    return;
                }
                if (!data) {
                    showPollProgress('ZigBee-Modus nicht aktiv');
                    return;
                }
                applyZigbeeStatusTable(data);
                const joined = zigbeeJsonIsTrue(data.joined) || data.status === 'joined';
                if (joined) {
                    scheduleNext = false;
                    finishZigbeeStatusPolling();
                    if (statusDiv) {
                        statusDiv.innerHTML = '<p class="zigbee-status-success">Gepaart (ZigBee-Config in RTC gültig) ' +
                            '(' + round + '/' + roundsMax + ').</p>';
                    }
                } else if (round >= roundsMax) {
                    scheduleNext = false;
                    finishZigbeeStatusPolling();
                    if (statusDiv) {
                        statusDiv.innerHTML = '<p class="zigbee-status-warn">Timeout: Noch kein Join im Gerätestatus (' +
                            round + '/' + roundsMax + '). ' +
                            'Zigbee2MQTT kann bereits „joined“ melden – Reboot oder später erneut prüfen.</p>';
                    }
                } else {
                    showPollProgress('noch nicht gepaart (RTC)');
                }
            })
            .catch(function(error) {
                if (seq !== zigbeeStatusFetchSeq) {
                    return;
                }
                if (round >= roundsMax) {
                    scheduleNext = false;
                    finishZigbeeStatusPolling();
                    if (statusDiv) {
                        statusDiv.innerHTML = '<p class="zigbee-status-error">Fehler beim Laden des Status (' +
                            round + '/' + roundsMax + '): ' +
                            (error && error.message ? error.message : 'unbekannt') + '</p>';
                    }
                    return;
                }
                let hint;
                if (error && error.name === 'AbortError') {
                    hint = 'keine Antwort innerhalb ' + (ZIGBEE_STATUS_LONG_POLL_TIMEOUT_MS / 1000) +
                        ' s (Long-Poll)';
                } else if (error && (error.message === 'Failed to fetch' || error.message === 'NetworkError when attempting to fetch resource.')) {
                    hint = 'keine Antwort (Firefox: 0 B)';
                } else {
                    hint = error.message;
                }
                showPollProgress(hint);
            })
            .finally(function() {
                if (scheduleNext && zigbeeStatusPollingActive) {
                    scheduleNextPoll();
                }
            });
    }

    pollOnce();
}

function zigbeeDisplayOrDash(value, emptyValues) {
    if (value === undefined || value === null) {
        return '-';
    }
    const s = String(value).trim();
    if (s.length === 0) {
        return '-';
    }
    const upper = s.toUpperCase();
    if (emptyValues && emptyValues.indexOf(upper) >= 0) {
        return '-';
    }
    return s;
}

function applyZigbeeStatusTable(data) {
    const statusCell = document.getElementById('zigbeeStatusCell');
    const addrCell = document.getElementById('zigbeeNetworkAddrCell');
    const panCell = document.getElementById('zigbeePanIdCell');
    const channelCell = document.getElementById('zigbeeChannelCell');
    const extCell = document.getElementById('zigbeeExtendedAddrCell');
    if (!statusCell) {
        return;
    }
    statusCell.textContent = zigbeeStatusLabelFromJson(data);
    if (addrCell) {
        addrCell.textContent = zigbeeDisplayOrDash(data.network_addr, ['0XFFFF', '0X0000']);
    }
    if (panCell) {
        panCell.textContent = zigbeeDisplayOrDash(data.pan_id, ['0X0000', '0XFFFF']);
    }
    if (channelCell) {
        const ch = data.channel;
        if (ch === 0 || ch === '0') {
            channelCell.textContent = '-';
        } else {
            channelCell.textContent = String(ch);
        }
    }
    if (extCell) {
        extCell.textContent = zigbeeDisplayOrDash(data.extended_addr, [
            '0X0000000000000000', '0X0'
        ]);
    }
}

// ZigBee-Status aktualisieren (GET /zigbee/status → Tabelle)
function updateZigbeeStatus() {
    zigbeeStatusFetchSeq += 1;
    const seq = zigbeeStatusFetchSeq;
    const statusDiv = document.getElementById('zigbeeActionStatus');
    if (statusDiv) {
        statusDiv.innerHTML = '<p class="zigbee-status-muted">Lade ZigBee-Status...</p>';
    }
    fetchZigbeeStatusJson()
        .then(data => {
            if (seq !== zigbeeStatusFetchSeq) {
                return;
            }
            if (!data) {
                if (statusDiv) {
                    statusDiv.innerHTML = '<p class="zigbee-status-muted">ZigBee ist nicht aktiv</p>';
                }
                return;
            }
            applyZigbeeStatusTable(data);
            if (statusDiv) {
                statusDiv.innerHTML = '';
            }
        })
        .catch(error => {
            if (seq !== zigbeeStatusFetchSeq) {
                return;
            }
            if (statusDiv) {
                statusDiv.innerHTML = '<p class="zigbee-status-error">Fehler beim Laden des Status: ' + error.message + '</p>';
            }
        });
}

// ZigBee Factory-Reset
function zigbeeFactoryReset() {
    if (!isWifiStaConnected()) {
        alert('ZigBee-Factory-Reset ist nur im WLAN (STA) möglich, nicht im Einrichtungs-Hotspot (AP).');
        return;
    }
    if (!confirm("Möchten Sie wirklich einen Factory-Reset für ZigBee durchführen?\n\nDies löscht alle ZigBee-Netzwerkdaten und setzt ZigBee auf 'factory-new' zurück.")) {
        return;
    }
    
    const statusDiv = document.getElementById('zigbeeActionStatus');
    const statusSpan = document.getElementById('zigbeeFactoryResetStatus');
    const resetButton = document.getElementById('zigbeeFactoryResetBtn');
    
    // Button deaktivieren und Status anzeigen
    if (resetButton) {
        resetButton.disabled = true;
    }
    if (statusSpan) {
        statusSpan.innerHTML = '<span class="text-info"><span class="spinner-border spinner-border-sm me-1" role="status" aria-hidden="true"></span>läuft...</span>';
    }
    if (statusDiv) {
        statusDiv.innerHTML = '<p class="zigbee-status-info">Factory-Reset wird durchgeführt...</p>';
    }
    
    // Aktuelles Admin-Passwort für Basic-Auth
    postTransferAction('/zigbee/action', { cmd: 'factory-reset' })
    .then(response => {
        if (!response.ok) {
            if (response.status === 401) {
                throw new Error('Authentifizierung fehlgeschlagen. Bitte Seite neu laden.');
            }
            return response.json().then(data => {
                throw new Error(data.message || 'Fehler beim Factory-Reset');
            });
        }
        return response.json();
    })
    .then(data => {
        // Erfolg: Zeige persistentes Alert-Fenster (wie bei Config-Speicherung)
        // Entferne alte Erfolgs-Alerts (falls vorhanden)
        const oldAlerts = document.querySelectorAll('.alert-success');
        oldAlerts.forEach(alert => alert.remove());
        
        // Erstelle neues Alert-Fenster
        const successAlert = document.createElement('div');
        successAlert.className = 'alert alert-success alert-dismissible fade show';
        successAlert.setAttribute('role', 'alert');
        successAlert.style.cssText = 'margin-bottom: 20px;';
        
        let alertMessage = `<strong>✓ ${data.message || 'Factory-Reset erfolgreich.'}</strong><br>`;
        alertMessage += `Alle ZigBee-Netzwerkdaten wurden gelöscht.<br>`;
        alertMessage += `<strong>Bitte betätigen Sie den "Reboot"-Button, damit der ZigBee-Stack sauber initialisiert wird.</strong><br><br>`;
        
        // Zeige empfangene JSON-Daten (für Debugging)
        alertMessage += `<details style="margin-top: 10px;">`;
        alertMessage += `<summary style="cursor: pointer; color: #667eea;">📋 Empfangene JSON-Daten anzeigen (Debugging)</summary>`;
        alertMessage += `<pre style="background: #f8f9fa; padding: 10px; border-radius: 4px; margin-top: 10px; font-size: 0.85em; overflow-x: auto;">`;
        alertMessage += JSON.stringify(data, null, 2);
        alertMessage += `</pre>`;
        alertMessage += `</details>`;
        
        alertMessage += `<button type="button" class="btn-close" data-bs-dismiss="alert" aria-label="Close"></button>`;
        
        successAlert.innerHTML = alertMessage;
        
        // Füge Alert am Anfang des Containers ein
        const container = document.querySelector('.config-container');
        if (container) {
            container.insertBefore(successAlert, container.firstChild);
        } else {
            // Fallback: Am Anfang des Body einfügen
            document.body.insertBefore(successAlert, document.body.firstChild);
        }
        
        // Scroll zum Alert
        successAlert.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
        
        // Status neben Button anzeigen
        if (statusSpan) {
            statusSpan.innerHTML = '<span class="text-success">✓ OK</span>';
        }
        stopZigbeeStatusPolling();
        // Ein Fetch nach kurzer Pause (Stack/NVS); kein Doppel-Fetch (Race bei out-of-order Antworten)
        setTimeout(updateZigbeeStatus, 800);
        
        // Status-Span nach 5 Sekunden wieder leeren
        setTimeout(() => {
            if (statusSpan) {
                statusSpan.innerHTML = '';
            }
        }, 5000);
    })
    .catch(error => {
        // Fehler: Status neben Button anzeigen
        if (statusSpan) {
            statusSpan.innerHTML = '<span class="text-danger">✗ Fehler</span>';
        }
        if (statusDiv) {
            statusDiv.innerHTML = `<p class="zigbee-status-error">Fehler: ${error.message}</p>`;
        }
        // Status-Span nach 5 Sekunden wieder leeren
        setTimeout(() => {
            if (statusSpan) {
                statusSpan.innerHTML = '';
            }
        }, 5000);
    })
    .finally(() => {
        // Button wieder aktivieren
        if (resetButton) {
            resetButton.disabled = false;
        }
    });
}

// ZigBee Pairing starten
function zigbeeStartPairing() {
    if (!isWifiStaConnected()) {
        alert('ZigBee-Pairing ist nur im WLAN (STA) möglich, nicht im Einrichtungs-Hotspot (AP).');
        return;
    }
    if (!confirm("Möchten Sie das ZigBee-Pairing jetzt starten?\n\nStellen Sie sicher, dass der Coordinator im 'Permit Join' Modus ist.")) {
        return;
    }

    stopZigbeeStatusPolling();
    const statusDiv = document.getElementById('zigbeeActionStatus');
    const statusCell = document.getElementById('zigbeeStatusCell');
    if (statusDiv) {
        statusDiv.innerHTML = '<p class="zigbee-status-info">Pairing wird gestartet...</p>';
    }

    postTransferAction('/zigbee/action', { cmd: 'start-pairing' })
    .then(response => {
        if (!response.ok) {
            if (response.status === 401) {
                throw new Error('Authentifizierung fehlgeschlagen. Bitte Seite neu laden.');
            }
            return response.json().then(data => {
                throw new Error(data.message || 'Fehler beim Starten des Pairings');
            });
        }
        return response.json();
    })
    .then(data => {
        if (statusCell) {
            statusCell.textContent = 'Pairing läuft…';
        }
        if (statusDiv) {
            statusDiv.innerHTML = '<p class="zigbee-status-info">Pairing/Übertragung läuft im Hintergrund (wie Timer-Wake, bis ca. 3 Min.)…</p>';
        }
        startZigbeeStatusLongPoll(ZIGBEE_STATUS_LONG_POLL_MAX_ROUNDS);
    })
    .catch(error => {
        finishZigbeeStatusPolling();
        if (statusDiv) {
            statusDiv.innerHTML = '<p class="zigbee-status-error">Fehler: ' + error.message + '</p>';
        }
        fetchZigbeeStatusJson().then(function(data) {
            if (data) {
                applyZigbeeStatusTable(data);
            }
        });
    });
}

// ============================================
// Transfer-Actions (POST + Basic Auth)
// ============================================

function getConfigAuthHeader() {
    const adminpassEl = document.getElementById('adminpass');
    const pass = adminpassEl ? adminpassEl.value : '';
    return 'Basic ' + btoa('admin:' + pass);
}

function postTransferAction(url, fields) {
    const params = new URLSearchParams();
    Object.keys(fields).forEach(function(key) {
        params.append(key, fields[key]);
    });
    return fetch(url, {
        method: 'POST',
        headers: {
            'Authorization': getConfigAuthHeader(),
            'Content-Type': 'application/x-www-form-urlencoded'
        },
        body: params.toString()
    });
}

function applyMqttTestButtonState() {
    const hidden = document.getElementById('wifi_sta_connected');
    const btn = document.getElementById('mqttTestBtn');
    const hint = document.getElementById('mqttTestHint');
    const staOk = hidden && hidden.value === '1';
    if (btn) {
        btn.disabled = !staOk;
    }
    if (hint) {
        hint.style.display = staOk ? 'none' : 'block';
    }
}

function mqttTestServer() {
    const statusEl = document.getElementById('mqttTestStatus');
    const btn = document.getElementById('mqttTestBtn');
    const host = (document.getElementById('mqtt_host') || {}).value || '';
    const port = (document.getElementById('mqtt_port') || {}).value || '';
    const username = (document.getElementById('mqtt_username') || {}).value || '';
    const password = (document.getElementById('mqtt_password') || {}).value || '';

    if (btn) btn.disabled = true;
    if (statusEl) {
        statusEl.innerHTML = '<span class="text-info">Teste Verbindung...</span>';
    }

    postTransferAction('/mqtt/action', {
        cmd: 'servertest',
        host: host,
        port: port,
        username: username,
        password: password
    })
    .then(function(response) {
        if (!response.ok) {
            if (response.status === 401) {
                throw new Error('Authentifizierung fehlgeschlagen. Bitte Seite neu laden.');
            }
            return response.json().then(function(data) {
                throw new Error(data.message || ('HTTP ' + response.status));
            });
        }
        return response.json();
    })
    .then(function(data) {
        if (!statusEl) return;
        let text = data.message || 'MQTT-Server connect: OK';
        if (data.broker_version) {
            text += ' (' + data.broker_version + ')';
        }
        statusEl.innerHTML = '<span class="text-success">' + text + '</span>';
    })
    .catch(function(error) {
        if (statusEl) {
            const msg = error.message || 'Unbekannter Fehler';
            if (msg.indexOf('401') >= 0 || msg.indexOf('Authentifizierung') >= 0) {
                statusEl.innerHTML = '<span class="text-danger">Authentifizierung fehlgeschlagen. Seite neu laden.</span>';
            } else {
                statusEl.innerHTML = '<span class="text-danger">' + msg + '</span>';
            }
        }
    })
    .finally(function() {
        applyMqttTestButtonState();
    });
}

// ============================================
// BLE-Konfiguration
// ============================================

function toggleBleConfigPanel() {
    const collapse = document.getElementById('bleConfigCollapse');
    const toggleBtn = document.getElementById('bleConfigToggle');
    
    if (collapse.style.display === 'none') {
        collapse.style.display = 'block';
        toggleBtn.textContent = '📶 BLE-Einstellungen... (ausblenden)';
        updateBleStatus();
    } else {
        collapse.style.display = 'none';
        toggleBtn.textContent = '📶 BLE-Einstellungen...';
    }
}

function updateBleStatus() {
    fetch('/ble/status')
    .then(response => response.json())
    .then(data => {
        const statusText = document.getElementById('bleStatusText');
        if (statusText) {
            if (data.advertising) {
                statusText.innerHTML = '<span class="text-warning">Advertising...</span>';
            } else if (data.connected) {
                statusText.innerHTML = '<span class="text-success">Verbunden</span>';
            } else if (data.initialized) {
                statusText.innerHTML = '<span class="text-info">Initialisiert</span>';
            } else {
                statusText.innerHTML = '<span class="text-muted">Nicht aktiv</span>';
            }
        }
    })
    .catch(error => {
        const statusText = document.getElementById('bleStatusText');
        if (statusText) {
            statusText.innerHTML = '<span class="text-muted">Nicht verfügbar</span>';
        }
    });
}

function blePairing() {
    const statusSpan = document.getElementById('blePairingStatus');
    const btn = document.getElementById('blePairingBtn');
    
    if (btn) btn.disabled = true;
    if (statusSpan) statusSpan.innerHTML = '<span class="text-info">Starte Advertising...</span>';

    postTransferAction('/ble/action', { cmd: 'start-pairing' })
    .then(response => {
        if (response.status === 401) {
            throw new Error('Authentifizierung fehlgeschlagen. Bitte Seite neu laden.');
        }
        if (!response.ok) throw new Error('HTTP ' + response.status);
        return response.json();
    })
    .then(data => {
        let count = 90;
        const msg = 'Jetzt im Node-RED Config-Node das Gerät wählen.';
        if (statusSpan) {
            statusSpan.innerHTML = '<span class="text-success">Advertising aktiv (' + count + ' s). ' + msg + '</span>';
        }
        const countdownInterval = setInterval(function() {
            count--;
            if (statusSpan) {
                statusSpan.innerHTML = '<span class="text-success">Advertising aktiv (' + count + ' s). ' + msg + '</span>';
            }
            if (count <= 0) {
                clearInterval(countdownInterval);
                updateBleStatus();
                if (btn) btn.disabled = false;
                if (statusSpan) statusSpan.innerHTML = '';
            }
        }, 1000);
        /* Fallback: nach 95 s aufräumen, falls Countdown ausbleibt */
        setTimeout(function() {
            clearInterval(countdownInterval);
            updateBleStatus();
            if (btn) btn.disabled = false;
            if (statusSpan) statusSpan.innerHTML = '';
        }, 95000);
    })
    .catch(error => {
        if (statusSpan) {
            statusSpan.innerHTML = '<span class="text-danger">Fehler: ' + error.message + '</span>';
        }
        if (btn) btn.disabled = false;
    });
}

// Beim Laden der Seite: gespeicherten Modus merken, nur dessen Parameter-Sektion anzeigen
document.addEventListener('DOMContentLoaded', function() {
    const sel = document.getElementById('transfer_mode');
    if (sel) savedTransferMode = sel.value;
    toggleTransferConfig();
    applyMqttTestButtonState();
    applyZigbeeStaOnlyState();
});
