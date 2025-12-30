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

