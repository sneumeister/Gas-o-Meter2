# Zigbee Network Steering Fehler - Erklärung

## Übersicht

Die folgenden Fehlermeldungen im Serial-Log während des Network Steering sind **normal** und **erwartetes Verhalten**:

```
ZB_TRACE_LOG[0]: zdo/zdo_app.c:602    Have not got nwk key - authentication failed
ZB_TRACE_LOG[0]: nwk/nwk_main.c:4511    zb_nwk_do_leave param 15 rejoin 0
ZB_TRACE_LOG[0]: common/zb_address.c:1138    zb_address_delete ref 57
ZB_TRACE_LOG[0]: nwk/nwk_join.c:251    No dev for join
ZB_TRACE_LOG[0]: zdo/zdo_commissioning.c:212    Can't find PAN to join to! param 0
```

## Was passiert während Network Steering?

1. **Network Discovery**: Das Device scannt alle Zigbee-Kanäle (11-26) nach verfügbaren Netzwerken
2. **Versuch zu joinen**: Für jedes gefundene Netzwerk versucht das Device, beizutreten
3. **Fehlgeschlagene Versuche**: Wenn ein Netzwerk nicht kompatibel ist (falscher Network Key, Sicherheitseinstellungen, etc.), verlässt das Device das Netzwerk wieder
4. **Erfolgreicher Join**: Wenn ein kompatibles Netzwerk gefunden wird, tritt das Device bei

## Erklärung der Fehlermeldungen

### 1. "Have not got nwk key - authentication failed"
- **Bedeutung**: Das Device hat versucht, sich mit einem Netzwerk zu verbinden, aber keinen gültigen Network Key erhalten
- **Ursache**: Das Netzwerk verwendet eine andere Sicherheitskonfiguration oder das Device ist nicht berechtigt
- **Verhalten**: Normal - das Device verlässt das Netzwerk und versucht das nächste

### 2. "zb_nwk_do_leave param X rejoin 0"
- **Bedeutung**: Das Device verlässt aktiv ein Netzwerk (nach fehlgeschlagenem Join-Versuch)
- **Ursache**: Teil des normalen Network Steering Prozesses
- **Verhalten**: Normal - das Device bereinigt die Verbindung und versucht das nächste Netzwerk

### 3. "zb_address_delete ref X"
- **Bedeutung**: Netzwerk-Adressen werden aus der internen Tabelle gelöscht
- **Ursache**: Teil des Cleanup-Prozesses nach dem Verlassen eines Netzwerks
- **Verhalten**: Normal - Bereinigung von nicht mehr gültigen Adressen

### 4. "No dev for join" / "Can't find PAN to join to!"
- **Bedeutung**: Während des Scans wurden keine kompatiblen Netzwerke gefunden (in diesem Moment)
- **Ursache**: 
  - Coordinator ist noch nicht bereit (Permit Join nicht aktiviert)
  - Netzwerk ist auf einem anderen Kanal
  - Timing-Problem (Scan zu früh)
- **Verhalten**: Normal - das Device versucht es erneut (Retry-Mechanismus)

## Warum mehrere Versuche?

Das Device verwendet einen **Retry-Mechanismus**:
- **Versuch 1**: Fehlgeschlagen (Coordinator noch nicht bereit, falsches Netzwerk, etc.)
- **Versuch 2**: Erfolgreich (Coordinator ist jetzt bereit, richtiges Netzwerk gefunden)

Das ist **erwartetes Verhalten** und zeigt, dass der Retry-Mechanismus funktioniert.

## Wann sind diese Fehler problematisch?

Diese Fehler sind **NICHT problematisch**, wenn:
- ✅ Das Device schließlich erfolgreich joined (wie in Ihrem Log: "Pairing erfolgreich (Versuch 2)")
- ✅ Das Interview erfolgreich abgeschlossen wird (nach der Wartezeit-Implementierung)

Sie wären **problematisch**, wenn:
- ❌ Das Device **nie** erfolgreich joined (alle Versuche fehlschlagen)
- ❌ Das Device joined, aber das Interview **immer** fehlschlägt

## Zusammenfassung

- ✅ **Diese Fehler sind normal** während Network Steering
- ✅ **Retry-Mechanismus funktioniert** (Versuch 2 erfolgreich)
- ✅ **Hauptproblem war**: Device geht zu früh in Deep-Sleep (behoben durch Interview-Wartezeit)
- ✅ **Lösung**: 90 Sekunden Wartezeit nach erstem Pairing für Interview-Abschluss

Die Fehler zeigen, dass das Device korrekt verschiedene Netzwerke testet und schließlich das richtige findet.
