// Daten beim Laden aktualisieren
window.addEventListener('load', () => {
    // Daten werden vom Server geliefert
    console.log('Seite geladen');
    
    // WiFi-Info Style aus data-Attribut setzen
    const wifiInfoElement = document.getElementById('wifi_info');
    if (wifiInfoElement && wifiInfoElement.dataset.style) {
        wifiInfoElement.style.cssText = wifiInfoElement.dataset.style;
    }

    initCounterManualInput();
});

// Deep-Sleep-Funktion
function deepSleepDevice() {
    // Sicherheitsfrage
    if (!confirm("Möchten Sie das Gerät wirklich in Deep-Sleep versetzen?")) {
        return;  // Abbrechen, wenn Benutzer "Abbrechen" wählt
    }
    
    // Deep-Sleep-Button finden und deaktivieren
    const deepSleepButtonElement = document.querySelector('button[onclick="deepSleepDevice()"]');
    if (!deepSleepButtonElement) {
        console.error("Deep-Sleep-Button nicht gefunden!");
        return;
    }
    
    // Button sofort deaktivieren
    deepSleepButtonElement.disabled = true;
    deepSleepButtonElement.textContent = 'Deep-Sleep wird durchgeführt...';
    
    // POST-Request an /deepsleep senden
    const formData = new FormData();
    formData.append('cmd', 'deepsleep');
    
    fetch('/deepsleep', {
        method: 'POST',
        body: formData
    })
    .then(response => {
        // Request erfolgreich gesendet (ESP geht jetzt in Deep-Sleep)
        console.log("Deep-Sleep-Request erfolgreich gesendet");
        if (deepSleepButtonElement) {
            deepSleepButtonElement.textContent = 'Deep-Sleep aktiviert';
        }
    })
    .catch(error => {
        // Fehler ist erwartet, da Server nach Deep-Sleep offline geht
        console.log("Request-Fehler (erwartet nach Deep-Sleep):", error);
        if (deepSleepButtonElement) {
            deepSleepButtonElement.textContent = 'Deep-Sleep aktiviert';
        }
    });
}

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
                fetch('/', {
                    method: 'HEAD',
                    cache: 'no-cache',
                    signal: AbortSignal.timeout(2000)
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

function updateSliderFromCounterFields() {
    const leftInput = document.getElementById('counterLeft');
    const rightInput = document.getElementById('counterRight');
    const slider = document.getElementById('counterSlider');
    if (!leftInput || !rightInput || !slider) {
        return;
    }

    let leftValue = parseInt(leftInput.value, 10);
    let rightValue = parseInt(rightInput.value, 10);
    if (isNaN(leftValue)) leftValue = 0;
    if (isNaN(rightValue)) rightValue = 0;
    leftValue = Math.min(99999, Math.max(0, leftValue));
    rightValue = Math.min(99, Math.max(0, rightValue));
    slider.value = leftValue * 100 + rightValue;
}

function padCounterField(input) {
    if (!input) return;
    const maxLen = input.id === 'counterRight' ? 2 : 5;
    let value = parseInt(input.value, 10);
    if (isNaN(value)) value = 0;
    if (input.id === 'counterRight') {
        value = Math.min(99, Math.max(0, value));
    } else {
        value = Math.min(99999, Math.max(0, value));
    }
    input.value = String(value).padStart(maxLen, '0');
    updateSliderFromCounterFields();
}

function focusCounterField(input, selectAll) {
    if (!input) return;
    input.focus();
    if (selectAll) {
        input.select();
    }
}

function initCounterManualInput() {
    const leftInput = document.getElementById('counterLeft');
    const rightInput = document.getElementById('counterRight');
    if (!leftInput || !rightInput) {
        return;
    }

    function onDigitsInput(event) {
        const input = event.target;
        const maxLen = input.id === 'counterRight' ? 2 : 5;
        const digitsOnly = input.value.replace(/\D/g, '').slice(0, maxLen);
        if (input.value !== digitsOnly) {
            input.value = digitsOnly;
        }
        updateSliderFromCounterFields();
    }

    function onFocusSelect(event) {
        event.target.select();
    }

    function onBlurPad(event) {
        padCounterField(event.target);
    }

    function onKeyDownNavigate(event) {
        const input = event.target;
        const isLeft = input.id === 'counterLeft';
        const isRight = input.id === 'counterRight';
        const value = input.value;
        const selStart = input.selectionStart;
        const selEnd = input.selectionEnd;
        const allSelected = selStart === 0 && selEnd === value.length;
        const atEnd = selStart === value.length && selEnd === value.length;
        const atStart = selStart === 0 && selEnd === 0;

        // Dezimaltrenner: vom linken Feld nach rechts
        if (isLeft && (event.key === '.' || event.key === ',')) {
            event.preventDefault();
            padCounterField(leftInput);
            focusCounterField(rightInput, true);
            return;
        }

        // Pfeiltasten an Feldgrenzen
        if (isLeft && event.key === 'ArrowRight' && (atEnd || allSelected)) {
            event.preventDefault();
            focusCounterField(rightInput, true);
            return;
        }
        if (isRight && event.key === 'ArrowLeft' && (atStart || allSelected)) {
            event.preventDefault();
            focusCounterField(leftInput, true);
            return;
        }

        // Linkes Feld voll + weitere Ziffer → nach rechts und Ziffer einfügen
        if (isLeft && /^[0-9]$/.test(event.key) && !event.ctrlKey && !event.metaKey && !event.altKey) {
            const replacingAll = allSelected;
            const currentLen = replacingAll ? 0 : value.length;
            if (currentLen >= 5 && selStart === selEnd) {
                event.preventDefault();
                padCounterField(leftInput);
                rightInput.value = event.key;
                focusCounterField(rightInput, false);
                rightInput.setSelectionRange(1, 1);
                updateSliderFromCounterFields();
            }
        }
    }

    [leftInput, rightInput].forEach((input) => {
        input.addEventListener('input', onDigitsInput);
        input.addEventListener('focus', onFocusSelect);
        input.addEventListener('blur', onBlurPad);
        input.addEventListener('keydown', onKeyDownNavigate);
    });
}

// Counter-Interval für +/- Buttons
let counterInterval = null;
let counterHoldTimeout = null;
let counterActive = false;
let counterLastTouchStartMs = 0;
let counterSpeed = 350;
const COUNTER_HOLD_DELAY_MS = 450; // Erst nach Hold Repeat starten (kurzer Tap = genau 1 Schritt)
const COUNTER_INITIAL_SPEED = 350; // ms zwischen Repeat-Schritten am Anfang
const COUNTER_ACCELERATION = 40; // Beschleunigung erst nach einigen Repeat-Ticks
const COUNTER_MIN_SPEED = 50; // Nicht zu aggressiv (Mobil/Firefox)
const COUNTER_ACCEL_AFTER_TICKS = 4;
const COUNTER_TOUCH_MOUSE_GUARD_MS = 600; // mousedown nach touchstart ignorieren

function applyCounterStep(direction, leftId, rightId) {
    const leftInput = document.getElementById(leftId);
    const rightInput = document.getElementById(rightId);
    if (!leftInput || !rightInput) {
        return;
    }

    let leftValue = parseInt(leftInput.value, 10) || 0;
    let rightValue = parseInt(rightInput.value, 10) || 0;

    if (direction > 0) {
        rightValue++;
        if (rightValue > 99) {
            rightValue = 0;
            leftValue++;
            if (leftValue > 99999) {
                leftValue = 0;
            }
        }
    } else {
        rightValue--;
        if (rightValue < 0) {
            rightValue = 99;
            leftValue--;
            if (leftValue < 0) {
                leftValue = 99999;
            }
        }
    }

    leftInput.value = String(leftValue).padStart(5, '0');
    rightInput.value = String(rightValue).padStart(2, '0');

    const slider = document.getElementById('counterSlider');
    if (slider) {
        slider.value = leftValue * 100 + rightValue;
    }
}

function startCounterHold(direction, leftId, rightId, event) {
    // Mobil: touchstart + synthetisches mousedown → sonst Doppel-Schritt
    if (event) {
        if (event.type === 'touchstart') {
            counterLastTouchStartMs = Date.now();
            if (typeof event.preventDefault === 'function') {
                event.preventDefault();
            }
        } else if (event.type === 'mousedown' &&
                   (Date.now() - counterLastTouchStartMs) < COUNTER_TOUCH_MOUSE_GUARD_MS) {
            return;
        }
    }

    stopCounter();
    counterActive = true;
    counterSpeed = COUNTER_INITIAL_SPEED;

    // Kurzer Tap: genau ein Schritt
    applyCounterStep(direction, leftId, rightId);

    // Dauerdruck: Repeat erst nach Hold-Delay, Beschleunigung verzögert
    counterHoldTimeout = setTimeout(() => {
        if (!counterActive) {
            return;
        }
        let ticks = 0;

        function tick() {
            if (!counterActive) {
                return;
            }
            applyCounterStep(direction, leftId, rightId);
            ticks++;
            if (ticks >= COUNTER_ACCEL_AFTER_TICKS) {
                counterSpeed = Math.max(COUNTER_MIN_SPEED, counterSpeed - COUNTER_ACCELERATION);
            }
            clearInterval(counterInterval);
            counterInterval = setInterval(tick, counterSpeed);
        }

        counterInterval = setInterval(tick, counterSpeed);
    }, COUNTER_HOLD_DELAY_MS);
}

function startIncrement(leftId, rightId, event) {
    startCounterHold(1, leftId, rightId, event);
}

function startDecrement(leftId, rightId, event) {
    startCounterHold(-1, leftId, rightId, event);
}

function stopCounter() {
    counterActive = false;
    if (counterHoldTimeout) {
        clearTimeout(counterHoldTimeout);
        counterHoldTimeout = null;
    }
    if (counterInterval) {
        clearInterval(counterInterval);
        counterInterval = null;
    }
    counterSpeed = COUNTER_INITIAL_SPEED;
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

