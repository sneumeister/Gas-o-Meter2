const environmentSelect = document.querySelector("#environment");
const installTypeSelect = document.querySelector("#install-type");
const installWrapper = document.querySelector("#install-wrapper");
const installButton = document.querySelector("#install-button");
const selectionHelp = document.querySelector("#selection-help");
const tplWarning = document.querySelector("#tpl-warning");
const partialWarning = document.querySelector("#partial-warning");
const littlefsOption = installTypeSelect.querySelector(
  'option[value="littlefs"]',
);

const descriptions = {
  complete:
    "Complete überschreibt Bootloader, Partitionstabelle, Firmware und – bei PCB-Versionen – LittleFS. Gespeicherte Konfiguration und Zähler-Ringspeicher gehen verloren.",
  firmware:
    "Firmware aktualisiert nur den Programmcode ab 0x10000. LittleFS und Datenpartitionen bleiben unverändert.",
  littlefs:
    "LittleFS aktualisiert Web-UI und Default-Konfiguration ab 0x285000. Die aktuelle Gerätekonfiguration wird dabei überschrieben.",
};

function updateSelection() {
  const environment = environmentSelect.value;
  let installType = installTypeSelect.value;
  const isTplTest = environment === "TPL_test";

  installTypeSelect.disabled = !environment;
  littlefsOption.disabled = isTplTest;
  littlefsOption.hidden = isTplTest;
  tplWarning.hidden = !isTplTest;

  if (isTplTest && installType === "littlefs") {
    installTypeSelect.value = "";
    installType = "";
  }
  partialWarning.hidden = !["firmware", "littlefs"].includes(installType);

  if (!environment) {
    selectionHelp.textContent =
      "Wähle zuerst die passende Platinen- bzw. Testversion.";
  } else if (!installType) {
    selectionHelp.textContent = isTplTest
      ? "TPL_test unterstützt Complete oder Firmware; LittleFS ist nicht enthalten."
      : "Wähle Complete, Firmware oder LittleFS.";
  } else if (isTplTest && installType === "complete") {
    selectionHelp.textContent =
      "TPL_test Complete schreibt Bootloader, Partitionstabelle und Test-Firmware. Spätere Datenpartitionen und LittleFS sind nicht Bestandteil des Images.";
  } else {
    selectionHelp.textContent = descriptions[installType];
  }

  const selectionValid = Boolean(environment && installType);
  installWrapper.hidden = !selectionValid;
  installButton.toggleAttribute("disabled", !selectionValid);

  if (selectionValid) {
    const manifest = `./firmware/latest/${environment}/manifest-${installType}.json`;
    installButton.setAttribute("manifest", manifest);
  } else {
    installButton.removeAttribute("manifest");
  }
}

environmentSelect.addEventListener("change", () => {
  installTypeSelect.value = "";
  updateSelection();
});
installTypeSelect.addEventListener("change", updateSelection);

updateSelection();
