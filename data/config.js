// WiFi-Credentials dynamisch verwalten
let wifiCredentials = [];

// Beim Laden: Aktuelle Config laden und Event-Listener registrieren
window.addEventListener('load', () => {
    loadWifiCredentials();
    
    // Event-Listener für Admin-Passwort-Validierung
    const adminpassNew = document.getElementById('adminpass_new');
    const adminpassConfirm = document.getElementById('adminpass_confirm');
    
    if (adminpassNew && adminpassConfirm) {
        adminpassNew.addEventListener('input', checkAdminPassMatch);
        adminpassConfirm.addEventListener('input', checkAdminPassMatch);
    }
});

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

// Passwort-Feld zwischen Klartext und versteckt umschalten
function togglePassword(inputId, buttonId) {
    const input = document.getElementById(inputId);
    const button = document.getElementById(buttonId);
    const icon = document.getElementById(buttonId + 'Icon');
    
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
                        <button class="btn btn-outline-secondary" type="button" id="toggleWifiPass_${index}" onclick="togglePassword('wifi_password_${index}', 'toggleWifiPass_${index}')">
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
        // Einfach die Seite neu laden - die Config wird automatisch aus RTC-RAM geladen
        window.location.reload();
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
    
    // Sammle alle Formular-Daten
    const formData = {
        hostname: document.getElementById('hostname').value.trim(),
        adminpass: adminpass,
        wakeup_minutes: parseInt(document.getElementById('wakeup_minutes').value),
        transfer_minutes: parseInt(document.getElementById('transfer_minutes').value),
        adc_voltage_offset: parseFloat(document.getElementById('adc_voltage_offset').value),
        ntp_server: document.getElementById('ntp_server').value.trim(),
        wifiCredentials: []
    };
    
    // Sammle WiFi-Credentials aus den gerenderten Feldern
    const wifiContainer = document.getElementById('wifiCredentials');
    const wifiSets = wifiContainer.querySelectorAll('.wifi-credential-set');
    
    for (let i = 0; i < wifiSets.length; i++) {
        const ssid = document.getElementById(`wifi_ssid_${i}`).value.trim();
        const password = document.getElementById(`wifi_password_${i}`).value.trim();
        
        // Nur nicht-leere Sets hinzufügen (mindestens SSID muss vorhanden sein)
        if (ssid.length > 0) {
            formData.wifiCredentials.push({
                ssid: ssid,
                password: password  // Passwort kann leer sein (offenes Netzwerk)
            });
        }
    }
    
    // Prüfe, ob mindestens ein WiFi-Set vorhanden ist
    if (formData.wifiCredentials.length === 0) {
        alert("Bitte geben Sie mindestens ein WiFi-Set (SSID) an.");
        return;
    }
    
    // Validierung
    if (formData.hostname.length === 0) {
        alert("Bitte geben Sie einen Hostname an.");
        return;
    }
    
    if (formData.adminpass.length === 0) {
        alert("Bitte geben Sie ein Admin-Passwort an.");
        return;
    }
    
    // 2-stufige Sicherheitsabfrage
    const currentWifi = getCurrentWifiCredentials();
    const wifiChanged = hasWifiChanged(currentWifi, formData.wifiCredentials);
    
    if (wifiChanged) {
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
    
    fetch('/config/save', {
        method: 'POST',
        headers: {
            'Authorization': authHeader
        },
        body: formDataToSend
    })
    .then(response => {
        if (response.ok) {
            alert("Konfiguration erfolgreich gespeichert!");
            window.location.reload();
        } else {
            response.text().then(text => {
                alert("Fehler beim Speichern: " + text);
            });
        }
    })
    .catch(error => {
        alert("Fehler beim Speichern: " + error.message);
    });
}

function getCurrentWifiCredentials() {
    // Versuche, aktuelle WiFi-Credentials aus der Seite zu extrahieren
    // (werden vom Server als verstecktes Element eingefügt)
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

function hasWifiChanged(current, newCredentials) {
    // Prüfe, ob sich die WiFi-Credentials geändert haben
    if (current.length !== newCredentials.length) {
        return true;
    }
    
    // Prüfe jedes Set
    for (let i = 0; i < current.length; i++) {
        const found = newCredentials.find(cred => 
            cred.ssid === current[i].ssid && cred.password === current[i].password
        );
        if (!found) {
            return true;
        }
    }
    
    return false;
}

