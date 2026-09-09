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

const lang = document.documentElement.lang === "en" ? "en" : "de";

const messages = {
  de: {
    pickBoard: "Wähle zuerst die passende Platinen- bzw. Testversion.",
    pickInstall: "Wähle Complete, Firmware oder LittleFS.",
    tplPickInstall:
      "TPL_test unterstützt Complete oder Firmware; LittleFS ist nicht enthalten.",
    tplComplete:
      "TPL_test Complete schreibt Bootloader, Partitionstabelle und Test-Firmware. Spätere Datenpartitionen und LittleFS sind nicht Bestandteil des Images.",
    descriptions: {
      complete:
        "Complete überschreibt Bootloader, Partitionstabelle, Firmware und – bei PCB-Versionen – LittleFS. Gespeicherte Konfiguration und Zähler-Ringspeicher gehen verloren.",
      firmware:
        "Firmware aktualisiert nur den Programmcode ab 0x10000. LittleFS und Datenpartitionen bleiben unverändert.",
      littlefs:
        "LittleFS aktualisiert Web-UI und Default-Konfiguration ab 0x285000. Die aktuelle Gerätekonfiguration wird dabei überschrieben.",
    },
  },
  en: {
    pickBoard: "First select the matching board or test version.",
    pickInstall: "Choose Complete, Firmware, or LittleFS.",
    tplPickInstall:
      "TPL_test supports Complete or Firmware; LittleFS is not included.",
    tplComplete:
      "TPL_test Complete writes bootloader, partition table, and test firmware. Later data partitions and LittleFS are not part of the image.",
    descriptions: {
      complete:
        "Complete overwrites bootloader, partition table, firmware, and — on PCB versions — LittleFS. Saved configuration and the counter ring buffer are lost.",
      firmware:
        "Firmware updates only the program code from 0x10000. LittleFS and data partitions stay unchanged.",
      littlefs:
        "LittleFS updates the web UI and default configuration from 0x285000. The current device configuration is overwritten.",
    },
  },
};

const t = messages[lang];

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
    selectionHelp.textContent = t.pickBoard;
  } else if (!installType) {
    selectionHelp.textContent = isTplTest ? t.tplPickInstall : t.pickInstall;
  } else if (isTplTest && installType === "complete") {
    selectionHelp.textContent = t.tplComplete;
  } else {
    selectionHelp.textContent = t.descriptions[installType];
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
