let autoRefreshInterval = null;
let autoRefreshEnabled = false;

function autoRefresh() {
    if (autoRefreshEnabled) {
        clearInterval(autoRefreshInterval);
        autoRefreshEnabled = false;
        document.getElementById('autoRefreshStatus').textContent = 'Aus';
    } else {
        autoRefreshInterval = setInterval(() => {
            location.reload();
        }, 5000); // Alle 5 Sekunden
        autoRefreshEnabled = true;
        document.getElementById('autoRefreshStatus').textContent = 'An (5s)';
    }
}

// Daten beim Laden aktualisieren
window.addEventListener('load', () => {
    // Daten werden vom Server geliefert
    console.log('Seite geladen');
    
    // WiFi-Info Style aus data-Attribut setzen
    const wifiInfoElement = document.getElementById('wifi_info');
    if (wifiInfoElement && wifiInfoElement.dataset.style) {
        wifiInfoElement.style.cssText = wifiInfoElement.dataset.style;
    }
});

// Reboot-Funktion mit Countdown-Timer
let rebootCountdown = null;
let rebootButtonElement = null;

function rebootDevice() {
    // Sicherheitsfrage nur einmal anzeigen
    if (!confirm("Möchten Sie das Gerät wirklich neu starten?")) {
        return;  // Abbrechen, wenn Benutzer "Abbrechen" wählt
    }
    
    // Reboot-Button finden und deaktivieren
    rebootButtonElement = document.querySelector('button[onclick="rebootDevice()"]');
    if (!rebootButtonElement) {
        console.error("Reboot-Button nicht gefunden!");
        return;
    }
    
    // Button sofort deaktivieren
    rebootButtonElement.disabled = true;
    rebootButtonElement.textContent = 'Reboot wird durchgeführt...';
    
    // POST-Request an /reboot senden (ESP startet sofort neu)
    // Parameter cmd=reboot erforderlich (Schutz vor versehentlichem Aufruf)
    const formData = new FormData();
    formData.append('cmd', 'reboot');
    
    fetch('/reboot', {
        method: 'POST',
        body: formData
    })
    .then(response => {
        // Request erfolgreich gesendet (ESP rebootet jetzt)
        console.log("Reboot-Request erfolgreich gesendet");
    })
    .catch(error => {
        // Fehler ist erwartet, da Server nach Reboot offline geht
        console.log("Request-Fehler (erwartet nach Reboot):", error);
    });
    
    // Countdown-Timer starten (parallel zum Reboot)
    // Warte 3 Sekunden, dann starte Reload-Versuche
    let waitTime = 3;  // Sekunden bis zum ersten Reload-Versuch
    let retryCount = 0;
    const maxRetries = 20;  // Max. 20 Sekunden warten
    
    // Countdown anzeigen
    rebootCountdown = setInterval(() => {
        if (waitTime > 0) {
            // Warte noch auf ersten Reload-Versuch
            rebootButtonElement.textContent = `Warte auf Server-Neustart... ${waitTime}s`;
            waitTime--;
        } else {
            // Starte Reload-Versuche
            clearInterval(rebootCountdown);
            
            function tryReload() {
                retryCount++;
                if (rebootButtonElement) {
                    rebootButtonElement.textContent = `Warte auf Server... ${retryCount}s`;
                }
                
                // Versuche Seite neu zu laden
                fetch(window.location.href, { 
                    method: 'HEAD', 
                    cache: 'no-cache',
                    signal: AbortSignal.timeout(2000)  // 2 Sekunden Timeout
                })
                    .then(() => {
                        // Server ist wieder online → Seite neu laden
                        console.log("Server ist wieder online");
                        clearInterval(rebootCountdown);
                        if (rebootButtonElement) {
                            rebootButtonElement.textContent = 'Server online - Lade Seite neu...';
                        }
                        setTimeout(() => {
                            location.reload();
                        }, 500);
                    })
                    .catch((error) => {
                        // Server noch nicht online → erneut versuchen
                        if (retryCount < maxRetries) {
                            setTimeout(tryReload, 1000);  // Nach 1 Sekunde erneut versuchen
                        } else {
                            // Max. Versuche erreicht → trotzdem neu laden
                            console.log("Max. Versuche erreicht - lade Seite trotzdem neu");
                            clearInterval(rebootCountdown);
                            if (rebootButtonElement) {
                                rebootButtonElement.textContent = 'Lade Seite neu...';
                            }
                            setTimeout(() => {
                                location.reload();
                            }, 500);
                        }
                    });
            }
            
            // Starte ersten Reload-Versuch
            tryReload();
        }
    }, 1000);  // Jede Sekunde aktualisieren
}

// Zählerstand-Korrektur: Panel aufklappen/zuklappen
function toggleCounterCorrection() {
    const panel = document.getElementById('counterCorrectionPanel');
    const button = event.target;
    
    if (panel.style.display === 'none') {
        panel.style.display = 'block';
        button.textContent = '📝 Zählerstand korrigieren... (ausblenden)';
        
        // Aktuellen Wert in Felder eintragen
        const currentLeft = document.querySelector('.pulse-left').textContent.trim();
        const currentRight = document.querySelector('.pulse-right').textContent.trim();
        document.getElementById('counterLeft').value = currentLeft;
        document.getElementById('counterRight').value = currentRight;
        
        // Slider auf aktuellen Wert setzen
        const totalValue = parseInt(currentLeft) * 100 + parseInt(currentRight);
        document.getElementById('counterSlider').value = totalValue;
        
        // Timer stoppen (falls noch aktiv)
        stopCounter();
    } else {
        panel.style.display = 'none';
        button.textContent = '📝 Zählerstand korrigieren...';
        stopCounter();
    }
}

// Slider-Wert in Eingabefelder übernehmen
function updateCounterFromSlider() {
    const slider = document.getElementById('counterSlider');
    const totalValue = parseInt(slider.value) || 0;
    
    const leftValue = Math.floor(totalValue / 100);
    const rightValue = totalValue % 100;
    
    document.getElementById('counterLeft').value = String(leftValue).padStart(5, '0');
    document.getElementById('counterRight').value = String(rightValue).padStart(2, '0');
}

// Counter-Interval für +/- Buttons
let counterInterval = null;
let counterSpeed = 500; // Initial: 500ms zwischen Änderungen
const COUNTER_ACCELERATION = 60; // Beschleunigung beim Gedrückthalten (ms weniger pro Iteration)
const COUNTER_MIN_SPEED = 5; // Minimale Geschwindigkeit (ms) - entspricht 200x pro Sekunde (100x schneller als Start)

function startIncrement(leftId, rightId) {
    stopCounter(); // Sicherstellen, dass kein anderer Timer läuft
    counterSpeed = 500; // Reset Geschwindigkeit
    
    function increment() {
        const leftInput = document.getElementById(leftId);
        const rightInput = document.getElementById(rightId);
        
        let leftValue = parseInt(leftInput.value) || 0;
        let rightValue = parseInt(rightInput.value) || 0;
        
        // Erhöhe Nachkommastellen
        rightValue++;
        if (rightValue > 99) {
            rightValue = 0;
            leftValue++;
            if (leftValue > 99999) {
                leftValue = 0; // Wrap-around
            }
        }
        
        leftInput.value = String(leftValue).padStart(5, '0');
        rightInput.value = String(rightValue).padStart(2, '0');
        
        // Slider aktualisieren
        const totalValue = leftValue * 100 + rightValue;
        const slider = document.getElementById('counterSlider');
        if (slider) {
            slider.value = totalValue;
        }
        
        // Beschleunige beim Gedrückthalten
        counterSpeed = Math.max(COUNTER_MIN_SPEED, counterSpeed - COUNTER_ACCELERATION);
        
        // Neuen Timer mit aktualisierter Geschwindigkeit starten
        clearInterval(counterInterval);
        counterInterval = setInterval(increment, counterSpeed);
    }
    
    // Sofort einmal ausführen
    increment();
}

function startDecrement(leftId, rightId) {
    stopCounter();
    counterSpeed = 500;
    
    function decrement() {
        const leftInput = document.getElementById(leftId);
        const rightInput = document.getElementById(rightId);
        
        let leftValue = parseInt(leftInput.value) || 0;
        let rightValue = parseInt(rightInput.value) || 0;
        
        // Verringere Nachkommastellen
        rightValue--;
        if (rightValue < 0) {
            rightValue = 99;
            leftValue--;
            if (leftValue < 0) {
                leftValue = 99999; // Wrap-around
            }
        }
        
        leftInput.value = String(leftValue).padStart(5, '0');
        rightInput.value = String(rightValue).padStart(2, '0');
        
        // Slider aktualisieren
        const totalValue = leftValue * 100 + rightValue;
        const slider = document.getElementById('counterSlider');
        if (slider) {
            slider.value = totalValue;
        }
        
        // Beschleunige beim Gedrückthalten
        counterSpeed = Math.max(COUNTER_MIN_SPEED, counterSpeed - COUNTER_ACCELERATION);
        
        // Neuen Timer mit aktualisierter Geschwindigkeit starten
        clearInterval(counterInterval);
        counterInterval = setInterval(decrement, counterSpeed);
    }
    
    // Sofort einmal ausführen
    decrement();
}

function stopCounter() {
    if (counterInterval) {
        clearInterval(counterInterval);
        counterInterval = null;
    }
    counterSpeed = 500; // Reset
}

// Parameter-Tabelle: Panel aufklappen/zuklappen
function toggleParameters() {
    const panel = document.getElementById('parametersPanel');
    const button = event.target;
    
    if (panel.style.display === 'none') {
        panel.style.display = 'block';
        button.textContent = '📊 Parameter ausblenden';
    } else {
        panel.style.display = 'none';
        button.textContent = '📊 Parameter einblenden';
    }
}

// Zählerstand-Korrektur: Wert übernehmen
function applyCounterCorrection() {
    const leftValue = parseInt(document.getElementById('counterLeft').value) || 0;
    const rightValue = parseInt(document.getElementById('counterRight').value) || 0;
    
    // Validierung
    if (leftValue < 0 || leftValue > 99999) {
        alert("Vorkommastellen müssen zwischen 0 und 99999 liegen.");
        return;
    }
    
    if (rightValue < 0 || rightValue > 99) {
        alert("Nachkommastellen müssen zwischen 0 und 99 liegen.");
        return;
    }
    
    // Formatieren für Anzeige
    const formattedLeft = String(leftValue).padStart(5, '0');
    const formattedRight = String(rightValue).padStart(2, '0');
    const displayValue = formattedLeft + '.' + formattedRight;
    
    // Sicherheitsabfrage
    if (!confirm(`Möchten Sie den Zählerstand wirklich auf ${displayValue} setzen?\n\n` +
                `Dieser Wert wird in den Ring-Speicher geschrieben.`)) {
        return;
    }
    
    // Berechne Gesamtwert (z.B. 12345.67 = 1234567)
    const totalValue = leftValue * 100 + rightValue;
    
    // POST-Request an Server senden
    const formData = new FormData();
    formData.append('value', totalValue.toString());
    
    fetch('/counter/set', {
        method: 'POST',
        body: formData
    })
    .then(response => {
        if (response.ok) {
            return response.text();
        } else {
            return response.text().then(text => {
                throw new Error(text || 'Fehler beim Setzen des Zählerstands');
            });
        }
    })
    .then(message => {
        alert("Zählerstand erfolgreich gesetzt!\n\n" + message);
        // Seite neu laden, um neuen Wert anzuzeigen
        location.reload();
    })
    .catch(error => {
        alert("Fehler beim Setzen des Zählerstands:\n" + error.message);
    });
}

