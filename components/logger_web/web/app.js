// lastDSMRlogger SPA
// Date: 2026-11-15 · Build: 1
//
// Views share one page and are listed in the VIEWS registry: the tab bar
// (an iOS-style segmented control) is built from it, so a new feature only
// needs one registry entry plus a matching section in index.html and a
// command on the device. The Telegrams view opens first.
// All dynamic data travels over one WebSocket at /ws: the
// device pushes status and the telegram list, the browser sends
// form-urlencoded commands (cmd=config, cmd=save, cmd=telegram, cmd=raw,
// cmd=view, cmd=telegrams). Raw telegram bytes arrive as a binary frame
// behind their JSON header. Every rendering path uses textContent so
// telegram text is never interpreted as HTML. The active screen name is
// reported to the device so the on-device debug lines show what the browser
// is displaying. If the link drops, the client reconnects and re-requests
// what the current view needs.

"use strict";

const state =
{
  screen: "telegrams",
  config: null,
  selectedSeq: null,
  latestSeq: null,
  pendingRawSeq: null,
  ws: null,
  wsReady: false,
};

// One entry per feature view: the tab bar and the view switching are driven
// by this registry. To add a feature: add an entry here, add a matching
// <section class="card" hidden id="viewXxx"> to index.html and answer the
// view's command on the device.
const VIEWS =
[
  {id: "settings", label: "Settings", section: "viewSettings", command: "cmd=config"},
  {id: "telegrams", label: "Telegrams", section: "viewTelegrams", command: "cmd=telegrams"},
];

const el = {};

// Built once by buildTabBar(); maps a view id to its tab button.
const tabButtons = {};

function bindElements()
{
  const ids = ["tabBar", "statusStrip", "settingsMessage", "settingsForm",
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

//-- --------------------------------------------------------------------------
//-- WebSocket transport
//-- --------------------------------------------------------------------------

function wsSend(command)
{
  if (state.wsReady && (state.ws !== null))
  {
    state.ws.send(command);
  }
}

function viewById(name)
{
  return VIEWS.find((view) => view.id === name);
}

function requestCurrentView()
{
  reportActiveScreen();

  const view = viewById(state.screen);

  if (view !== undefined)
  {
    wsSend(view.command);
  }
}

function wsConnect()
{
  const proto = (location.protocol === "https:") ? "wss:" : "ws:";
  const socket = new WebSocket(proto + "//" + location.host + "/ws");
  state.ws = socket;

  socket.binaryType = "arraybuffer";

  socket.onopen = () =>
  {
    state.wsReady = true;
    el.statusStrip.textContent = "Connected to the logger.";
    requestCurrentView();
  };

  socket.onclose = () =>
  {
    state.wsReady = false;
    state.ws = null;
    el.statusStrip.textContent = "Connection to the logger lost: reconnecting...";
    setTimeout(wsConnect, 2000);
  };

  socket.onerror = () =>
  {
    try
    {
      socket.close();
    }
    catch (error)
    {
      // The close handler schedules the reconnect.
    }
  };

  socket.onmessage = (event) =>
  {
    if (typeof event.data === "string")
    {
      try
      {
        handleTextMessage(JSON.parse(event.data));
      }
      catch (error)
      {
        // Ignore anything that is not a valid protocol message.
      }
    }
    else
    {
      handleRawFrame(event.data);
    }
  };
}

function handleTextMessage(message)
{
  switch (message.type)
  {
    case "status":
      renderStatus(message);
      break;

    case "telegrams":
      renderTelegramList(message.telegrams || []);
      break;

    case "config":
      renderSettings(message);
      break;

    case "telegram":
      renderDetail(message);
      break;

    case "raw":
      state.pendingRawSeq = message.sequence;
      break;

    case "save":
      handleSaveResult(message);
      break;

    case "error":
      handleServerError(message);
      break;

    default:
      break;
  }
}

function handleRawFrame(buffer)
{
  if (state.pendingRawSeq === null)
  {
    return;
  }

  // textContent preserves the original line endings and never interprets
  // the telegram text as HTML.
  el.rawView.textContent = new TextDecoder("utf-8").decode(buffer);
  state.pendingRawSeq = null;
}

function handleServerError(message)
{
  if (message.request === "telegram")
  {
    el.telegramDetail.hidden = true;
    setTelegramState(message.error || "Telegram unavailable.");
  }
  else if (message.request === "raw")
  {
    el.rawView.textContent = "(" + (message.error || "raw bytes unavailable") + ")";
    state.pendingRawSeq = null;
  }
  else if (message.request === "save")
  {
    el.settingsMessage.textContent = "Rejected: " + message.error + ". The active configuration is preserved.";
  }
  else
  {
    el.settingsMessage.textContent = "Device error: " + message.error;
  }
}

function reportActiveScreen()
{
  wsSend("cmd=view&screen=" + encodeURIComponent(state.screen));
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

function renderStatus(status)
{
  let text = "State: " + (STATE_LABELS[status.state] || status.state);

  if (status.activeProfileId)
  {
    text += "  ·  profile " + status.activeProfileId;
  }

  if (status.receiverRunning)
  {
    // A test selection has no UART: say so instead of showing GPIO -1.
    text += status.testMode ? "  ·  test data (no UART)" : "  ·  RX GPIO " + status.rxGpio;
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

function renderSettings(message)
{
  state.config = message;

  const choices = message.choices;
  const settings = message.settings;

  if (message.hasStored)
  {
    el.settingsMessage.textContent = "Stored configuration shown. It is applied as one operation and survives a power cycle.";
  }
  else if (message.hasActive)
  {
    el.settingsMessage.textContent = "Active configuration shown; it could not be read back from storage.";
  }
  else
  {
    el.settingsMessage.textContent = "No valid configuration yet: reception awaits a valid configuration below.";
  }

  // The RX dropdown offers every eligible J5 input plus the test selection.
  const testGpio = (choices.testGpio === undefined) ? -1 : choices.testGpio;
  const gpioValues = (choices.gpios || []).concat([testGpio]);
  const gpioLabels = (choices.gpios || []).map((gpio) => "GPIO" + gpio).concat(["Test data (no UART)"]);
  fillSelect(el.selGpio, gpioValues, gpioLabels);
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
    el.selCountry.value = (choices.countries && choices.countries[0]) || "NL";
    fillProfiles(el.selCountry.value, null);
  }
}

function saveSettings(event)
{
  event.preventDefault();
  el.settingsMessage.textContent = "Saving...";

  const body = "cmd=save" +
               "&rxGpio=" + encodeURIComponent(el.selGpio.value) +
               "&country=" + encodeURIComponent(el.selCountry.value) +
               "&profileId=" + encodeURIComponent(el.selProfile.value) +
               "&mbus1=" + encodeURIComponent(el.selMbus1.value) +
               "&mbus2=" + encodeURIComponent(el.selMbus2.value) +
               "&mbus3=" + encodeURIComponent(el.selMbus3.value) +
               "&mbus4=" + encodeURIComponent(el.selMbus4.value);

  wsSend(body);
}

function handleSaveResult(message)
{
  if (message.ok)
  {
    el.settingsMessage.textContent = "Configuration activated and stored.";
  }
  else
  {
    el.settingsMessage.textContent = "Rejected: " + message.error + ". The active configuration is preserved.";
  }
}

//-- --------------------------------------------------------------------------
//-- Telegrams view
//-- --------------------------------------------------------------------------

function setTelegramState(message)
{
  el.telegramState.textContent = message;
}

function renderTelegramList(telegrams)
{
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

function selectTelegram(sequence)
{
  state.selectedSeq = sequence;
  el.agedOutNotice.hidden = true;
  el.telegramDetail.hidden = false;
  el.telegramList.value = String(sequence);
  wsSend("cmd=telegram&seq=" + encodeURIComponent(sequence));
  wsSend("cmd=raw&seq=" + encodeURIComponent(sequence));
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
}

//-- --------------------------------------------------------------------------
//-- View switching and boot
//-- --------------------------------------------------------------------------

// Builds the segmented control from the VIEWS registry: every future
// feature button gets the same pill styling and click wiring for free.
function buildTabBar()
{
  el.tabBar.textContent = "";

  VIEWS.forEach((view) =>
  {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "tab";
    button.textContent = view.label;
    button.setAttribute("role", "tab");
    button.addEventListener("click", () => showScreen(view.id));
    el.tabBar.appendChild(button);
    tabButtons[view.id] = button;
  });
}

function showScreen(name)
{
  const view = viewById(name);

  if (view === undefined)
  {
    return;
  }

  state.screen = view.id;

  VIEWS.forEach((entry) =>
  {
    el[entry.section].hidden = (entry.id !== view.id);
    tabButtons[entry.id].classList.toggle("active", entry.id === view.id);
    tabButtons[entry.id].setAttribute("aria-selected", String(entry.id === view.id));
  });

  wsSend(view.command);
  reportActiveScreen();
}

function boot()
{
  bindElements();
  buildTabBar();
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

  wsConnect();
  showScreen("telegrams");
}

document.addEventListener("DOMContentLoaded", boot);
