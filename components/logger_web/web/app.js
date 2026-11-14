// lastDSMRlogger SPA
// Date: 2026-11-14 · Build: 1
//
// Two views share one page: Settings and Telegrams. The page polls the
// device; every rendering path uses textContent so telegram text is never
// interpreted as HTML. The active screen name is reported to the device so
// the on-device debug lines show what the browser is displaying.

"use strict";

const POLL_MS = 2000;

const state =
{
  screen: "settings",
  config: null,
  selectedSeq: null,
  latestSeq: null,
  telegramTimer: null,
};

const el = {};

function bindElements()
{
  const ids = ["tabSettings", "tabTelegrams", "statusStrip", "settingsMessage", "settingsForm",
               "selGpio", "selCountry", "selProfile", "selMbus1", "selMbus2", "selMbus3", "selMbus4",
               "saveSettings", "telegramState", "telegramList", "agedOutNotice", "backToLatest",
               "telegramDetail", "recordMeta", "recordNotes", "summaryTable", "rawView", "footerInfo",
               "viewSettings", "viewTelegrams"];

  ids.forEach((id) =>
  {
    el[id] = document.getElementById(id);
  });
}

function formatUptime(ms)
{
  const totalSeconds = Math.floor(ms / 1000);
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  return hours + "h " + String(minutes).padStart(2, "0") + "m " + String(seconds).padStart(2, "0") + "s";
}

async function postForm(path, body)
{
  return fetch(path,
  {
    method: "POST",
    headers: {"Content-Type": "application/x-www-form-urlencoded"},
    body: body,
  });
}

function reportActiveScreen()
{
  postForm("/api/view", "screen=" + encodeURIComponent(state.screen)).catch(() => {});
}

//-- --------------------------------------------------------------------------
//-- Status strip
//-- --------------------------------------------------------------------------

const STATE_LABELS =
{
  awaitingConfiguration: "Input awaiting configuration",
  awaitingData: "Input awaiting data",
  ok: "Accepted telegram",
  stale: "Stale last reading",
  error: "Receiver or parser error",
};

async function pollStatus()
{
  try
  {
    const response = await fetch("/api/status");
    const status = await response.json();

    let text = "State: " + (STATE_LABELS[status.state] || status.state);

    if (status.activeProfileId)
    {
      text += "  ·  profile " + status.activeProfileId;
    }

    if (status.receiverRunning)
    {
      text += "  ·  RX GPIO " + status.rxGpio;
    }

    // Wall clock time is unavailable in this release: show receive age and
    // uptime explicitly instead.
    text += "  ·  uptime " + formatUptime(status.uptimeMs);

    if (status.hasLastAccepted)
    {
      text += "  ·  last received " + formatUptime(status.lastAcceptedAgeMs) + " ago";
    }

    text += "  ·  accepted " + status.acceptedCount + ", rejected " + status.rejectedCount + ", data loss " + status.dataLossCount;

    if (status.lastDiagReason)
    {
      text += "  ·  last diagnostic: " + status.lastDiagReason;

      if (status.lastDiagObis)
      {
        text += " @ " + status.lastDiagObis;
      }
    }

    if (status.lastError)
    {
      text += "  ·  error: " + status.lastError;
    }

    el.statusStrip.textContent = text;
    el.statusStrip.classList.toggle("error", status.state === "error");
    el.footerInfo.textContent = "lastDSMRlogger · first release · telegram history stays in RAM";
  }
  catch (error)
  {
    el.statusStrip.textContent = "Cannot reach the logger: " + error;
  }
}

//-- --------------------------------------------------------------------------
//-- Settings view
//-- --------------------------------------------------------------------------

function fillSelect(select, values, labels)
{
  select.textContent = "";

  values.forEach((value, index) =>
  {
    const option = document.createElement("option");
    option.value = String(value);
    option.textContent = (labels !== undefined) ? labels[index] : String(value);
    select.appendChild(option);
  });
}

function mbusOptions(deviceTypes)
{
  const values = ["auto", "disabled"];
  const labels = ["Auto", "Disabled"];

  deviceTypes.forEach((type) =>
  {
    values.push("expected:" + type);
    labels.push(type.charAt(0).toUpperCase() + type.slice(1));
  });

  return {values: values, labels: labels};
}

function profilesForCountry(country)
{
  return (state.config.choices.profiles || []).filter((profile) => profile.country === country);
}

function fillProfiles(country, preferredId)
{
  const profiles = profilesForCountry(country);
  const values = profiles.map((profile) => profile.id);
  const labels = profiles.map((profile) => profile.label);
  fillSelect(el.selProfile, values, labels);

  if (preferredId && values.includes(preferredId))
  {
    el.selProfile.value = preferredId;
  }
}

async function loadSettings()
{
  try
  {
    const response = await fetch("/api/config");
    state.config = await response.json();
  }
  catch (error)
  {
    el.settingsMessage.textContent = "Cannot load the configuration: " + error;
    return;
  }

  const choices = state.config.choices;
  const settings = state.config.settings;

  el.settingsMessage.textContent = state.config.hasActive
    ? "Active configuration shown. It is applied as one operation and survives a power cycle."
    : "No valid configuration yet: reception awaits a valid configuration below.";

  fillSelect(el.selGpio, choices.gpios, choices.gpios.map((gpio) => "GPIO" + gpio));
  fillSelect(el.selCountry, choices.countries);

  const mbus = mbusOptions(choices.mbusTypes || []);

  [el.selMbus1, el.selMbus2, el.selMbus3, el.selMbus4].forEach((select) =>
  {
    fillSelect(select, mbus.values, mbus.labels);
  });

  if (settings)
  {
    el.selGpio.value = String(settings.rxGpio);
    el.selCountry.value = settings.country;
    fillProfiles(settings.country, settings.profileId);

    const channels = [el.selMbus1, el.selMbus2, el.selMbus3, el.selMbus4];

    (settings.mbus || []).forEach((selection, index) =>
    {
      if (channels[index] && mbus.values.includes(selection))
      {
        channels[index].value = selection;
      }
    });
  }
  else
  {
    el.selCountry.value = choices.countries[0] || "NL";
    fillProfiles(el.selCountry.value, null);
  }
}

async function saveSettings(event)
{
  event.preventDefault();
  el.settingsMessage.textContent = "Saving...";

  const body = "rxGpio=" + encodeURIComponent(el.selGpio.value) +
               "&country=" + encodeURIComponent(el.selCountry.value) +
               "&profileId=" + encodeURIComponent(el.selProfile.value) +
               "&mbus1=" + encodeURIComponent(el.selMbus1.value) +
               "&mbus2=" + encodeURIComponent(el.selMbus2.value) +
               "&mbus3=" + encodeURIComponent(el.selMbus3.value) +
               "&mbus4=" + encodeURIComponent(el.selMbus4.value);

  try
  {
    const response = await postForm("/api/config", body);
    const result = await response.json();

    if (result.ok)
    {
      el.settingsMessage.textContent = "Configuration activated and stored.";
      await loadSettings();
      await pollStatus();
    }
    else
    {
      el.settingsMessage.textContent = "Rejected: " + result.error + ". The active configuration is preserved.";
    }
  }
  catch (error)
  {
    el.settingsMessage.textContent = "Save failed: " + error + ". The active configuration is preserved.";
  }
}

//-- --------------------------------------------------------------------------
//-- Telegrams view
//-- --------------------------------------------------------------------------

function setTelegramState(message)
{
  el.telegramState.textContent = message;
}

async function pollTelegrams()
{
  try
  {
    const response = await fetch("/api/telegrams");
    const data = await response.json();
    const telegrams = data.telegrams || [];

    if (telegrams.length === 0)
    {
      el.telegramList.textContent = "";
      el.telegramDetail.hidden = true;
      el.agedOutNotice.hidden = true;
      setTelegramState("No telegram accepted yet.");
      return;
    }

    el.latestSeq = telegrams[0].sequence;

    el.telegramList.textContent = "";
    telegrams.forEach((telegram) =>
    {
      const option = document.createElement("option");
      option.value = String(telegram.sequence);
      const crc = telegram.crcVerified ? "crc ok" : "no crc";
      option.textContent = "#" + telegram.sequence + "  " + telegram.profileId +
                           "  v" + telegram.reportedVersion + "  " + crc +
                           "  at " + formatUptime(telegram.receivedUptimeMs);
      el.telegramList.appendChild(option);
    });

    const stillAvailable = telegrams.some((telegram) => telegram.sequence === state.selectedSeq);

    if (state.selectedSeq === null)
    {
      selectTelegram(el.latestSeq);
      return;
    }

    if (stillAvailable)
    {
      // A new arrival leaves the user's selected telegram in view.
      el.agedOutNotice.hidden = true;
      el.telegramList.value = String(state.selectedSeq);
    }
    else
    {
      el.agedOutNotice.hidden = false;
    }
  }
  catch (error)
  {
    setTelegramState("Cannot reach the logger: " + error);
  }
}

async function selectTelegram(sequence)
{
  state.selectedSeq = sequence;
  el.agedOutNotice.hidden = true;
  el.telegramDetail.hidden = false;

  try
  {
    const response = await fetch("/api/telegram/" + sequence);
    const detail = await response.json();

    if (detail.ok === false)
    {
      el.telegramDetail.hidden = true;
      setTelegramState(detail.error || "Telegram unavailable.");
      return;
    }

    renderDetail(detail);
  }
  catch (error)
  {
    setTelegramState("Cannot load the telegram: " + error);
  }
}

function renderDetail(detail)
{
  const meta = detail.meta;

  el.recordMeta.textContent = "#" + meta.sequence + " · captured " + formatUptime(meta.receivedUptimeMs) +
                              " after boot · profile " + meta.profileId +
                              " · meter version " + meta.reportedVersion +
                              " · " + (meta.crcVerified ? "CRC verified" : "CRC absent") +
                              " · " + meta.rawLength + " raw bytes";

  el.recordNotes.textContent = meta.notes ? ("Diagnostics: " + meta.notes) : "";

  const tbody = el.summaryTable.tBodies[0];
  tbody.textContent = "";

  const disabledMask = meta.disabledChannelsMask || 0;
  const presentChannels = new Set();
  const readingChannels = new Set();

  (detail.fields || []).forEach((field) =>
  {
    if (field.mbusChannel > 0)
    {
      presentChannels.add(field.mbusChannel);

      if (field.obis === ("0-" + field.mbusChannel + ":24.2.1") && field.value !== "")
      {
        readingChannels.add(field.mbusChannel);
      }
    }
  });

  (detail.fields || []).forEach((field) =>
  {
    // Disabled channels are omitted from the decoded summary while their
    // lines remain visible in the raw telegram below.
    if (field.mbusChannel > 0 && ((disabledMask >> field.mbusChannel) & 1))
    {
      return;
    }

    const row = document.createElement("tr");
    const cells = [
      field.obis,
      // An empty value string marks an unavailable reading; a measured zero
      // keeps its own text and stays distinguishable.
      (field.value === "") ? "(unavailable)" : field.value,
      field.unit,
      field.hasTimestamp ? field.timestamp : "",
      (field.mbusChannel > 0) ? ("ch " + field.mbusChannel) : "",
    ];

    cells.forEach((text) =>
    {
      const cell = document.createElement("td");
      cell.textContent = text;
      row.appendChild(cell);
    });

    tbody.appendChild(row);
  });

  // A channel that published fields but no reading is shown as unavailable.
  presentChannels.forEach((channel) =>
  {
    if ((disabledMask >> channel) & 1)
    {
      return;
    }

    if (!readingChannels.has(channel))
    {
      const row = document.createElement("tr");
      const cells = ["0-" + channel + ":24.2.1", "(unavailable)", "", "", "ch " + channel];

      cells.forEach((text) =>
      {
        const cell = document.createElement("td");
        cell.textContent = text;
        row.appendChild(cell);
      });

      tbody.appendChild(row);
    }
  });

  setTelegramState("");
  loadRaw(meta.sequence);
}

async function loadRaw(sequence)
{
  try
  {
    const response = await fetch("/api/telegram/" + sequence + "/raw");

    if (!response.ok)
    {
      el.rawView.textContent = "(raw bytes unavailable)";
      return;
    }

    // textContent preserves the original line endings and never interprets
    // the telegram text as HTML.
    el.rawView.textContent = await response.text();
  }
  catch (error)
  {
    el.rawView.textContent = "(cannot load raw bytes: " + error + ")";
  }
}

//-- --------------------------------------------------------------------------
//-- View switching and boot
//-- --------------------------------------------------------------------------

function showScreen(name)
{
  state.screen = name;
  el.viewSettings.hidden = (name !== "settings");
  el.viewTelegrams.hidden = (name !== "telegrams");
  el.tabSettings.classList.toggle("active", name === "settings");
  el.tabTelegrams.classList.toggle("active", name === "telegrams");

  if (name === "settings")
  {
    loadSettings();
  }
  else
  {
    pollTelegrams();
  }

  reportActiveScreen();
}

function startTimers()
{
  setInterval(pollStatus, POLL_MS);
  state.telegramTimer = setInterval(() =>
  {
    if (state.screen === "telegrams")
    {
      pollTelegrams();
    }
  }, POLL_MS);
}

function boot()
{
  bindElements();

  el.tabSettings.addEventListener("click", () => showScreen("settings"));
  el.tabTelegrams.addEventListener("click", () => showScreen("telegrams"));
  el.settingsForm.addEventListener("submit", saveSettings);

  el.selCountry.addEventListener("change", () =>
  {
    fillProfiles(el.selCountry.value, null);
  });

  el.telegramList.addEventListener("change", () =>
  {
    selectTelegram(Number(el.telegramList.value));
  });

  el.backToLatest.addEventListener("click", () =>
  {
    if (state.latestSeq !== null)
    {
      selectTelegram(state.latestSeq);
    }
  });

  pollStatus();
  showScreen("settings");
  startTimers();
}

document.addEventListener("DOMContentLoaded", boot);
