<!-- translation-source: PATCH-generic-ble-onMiss.md -->
<!-- translation-source-blob: f680643b86e20f970b8cb460b91f7f3d2b0110fe -->

[↓ Wechsel zu Deutsch](PATCH-generic-ble-onMiss.md)

# Patch: node-red-contrib-generic-ble – Crash “indexOf is not a function”

**Affected file (path depends on installation):**

```text
~/.node-red/node_modules/node-red-contrib-generic-ble/dist/noble/index.js
```

On Debian/system Node-RED (user `nodered`):

```text
/home/nodered/.node-red/node_modules/node-red-contrib-generic-ble/dist/noble/index.js
```

*Note:* Some installs use a different path. If unsure, find the file with:  
`find ~/.node-red -name "index.js" 2>/dev/null | xargs grep -l "_discoveredPeripheralUUids" 2>/dev/null`

---

## Replace with `index.js_patch` (generic-ble v4.0.3)

The package's original `index.js` is minified and awkward to edit by hand. **For node-red-contrib-generic-ble v4.0.3**, replace it with the already patched, readable copy:

- The repo includes `integration_templates/nodered/index.js_patch` (same content as original `index.js`, formatted, with the onMiss fix).
- Destination path as above (e.g.  
  `~/.node-red/node_modules/node-red-contrib-generic-ble/dist/noble/index.js`).

**Steps:**

1. Back up the old `index.js` (e.g. `index.js.bak`).
2. Copy `index.js_patch` from the gas-o-meter2 repo to `…/dist/noble/index.js` (adjust path).
3. Restart Node-RED.

This avoids manually inserting the snippet into the minified file.

---

## Snippet (insert at the start of `onMiss`)

`onMiss` starts roughly like this (around lines 58–65):

```javascript
onMiss(peripheral) {
    // INSERT HERE (very start of the function):
    if (!Array.isArray(this._discoveredPeripheralUUids)) {
        this._discoveredPeripheralUUids = [];
    }
    // … remaining existing code (e.g. this._discoveredPeripheralUUids.indexOf(...)) …
}
```

**Insert only this block** (3 lines), directly after `onMiss(peripheral) {`:

```javascript
    if (!Array.isArray(this._discoveredPeripheralUUids)) {
        this._discoveredPeripheralUUids = [];
    }
```

---

## Apply manually

1. Open the file in an editor (e.g. `nano` or `vim`).
2. Find the line with `onMiss(peripheral)` or `onMiss (peripheral)`.
3. Insert the three snippet lines **directly below** (after the opening `{`).
4. Save and restart Node-RED (e.g. `sudo systemctl restart nodered`).

After the patch, the “device missed” path should no longer crash; the flow can continue and deliver data on the next connect/notify.
