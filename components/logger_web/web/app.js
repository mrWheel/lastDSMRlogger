// lastDSMRlogger SPA
// Date: 2026-11-17 · Build: 13
//
// Views share one page and are listed in the VIEWS registry: the tab bar
// (an iOS-style segmented control) is built from it, so a new feature only
// needs one registry entry plus a matching section in index.html and a
// command on the device. The Telegrams view opens first.
// All dynamic data travels over one WebSocket at /ws: the
// device pushes status, the telegram list and the realtime power frame, the
// browser sends form-urlencoded commands (cmd=config, cmd=save,
// cmd=telegram, cmd=raw, cmd=view, cmd=telegrams, cmd=power). Raw telegram
// bytes arrive as a binary frame
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
  followLatest: false,
  pendingRawSeq: null,
  ws: null,
  wsReady: false,
  lastStatus: null,
  powerLastSeq: null,
  powerHistory: [],
};

// One entry per feature view: the tab bar and the view switching are driven
// by this registry. To add a feature: add an entry here, add a matching
// <section class="card" hidden id="viewXxx"> to index.html and answer the
// view's command on the device.
const VIEWS =
[
  {id: "settings", label: "Settings", section: "viewSettings", command: "cmd=config"},
  {id: "telegrams", label: "Telegrams", section: "viewTelegrams", command: "cmd=telegrams"},
  {id: "power", label: "Phase Load", section: "viewPower", command: "cmd=power"},
];

const el = {};

// Built once by buildTabBar(); maps a view id to its tab button.
const tabButtons = {};

function bindElements()
{
  const ids = ["tabBar", "statusStrip", "settingsMessage", "settingsForm",
               "headerVersion", "headerClock",
               "srcToggle", "srcLiveLabel", "srcTestLabel",
               "rxPolarityToggle", "polNormalLabel", "polInvertedLabel",
               "txPolarityToggle", "txPolNormalLabel", "txPolInvertedLabel", "inpInterval",
               "displayRotationToggle", "rotNormalLabel", "rotUpsideDownLabel",
               "selCountry", "selProfile", "selMbus1", "selMbus2", "selMbus3", "selMbus4",
               "saveSettings", "telegramState", "showTelegram", "telegramLog",
               "telegramDetail", "recordMeta", "recordNotes", "summaryTable", "rawView", "footerInfo",
               "powerPanel", "powerTelegram", "powerRows",
               "powerScaleLabel", "powerSpark",
               "viewSettings", "viewTelegrams", "viewPower"];

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
    updatePowerPolling();
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
        // Anything that is not a valid protocol message is ignored, but a
        // handler exception is surfaced in the console for field debugging.
        console.error("ws message handling failed:", error, event.data);
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

    case "power":
      renderPower(message);
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
  // A binary frame is only ever the answer to cmd=raw: fill the view even
  // when the header was missed, so the raw telegram can never stay blank.
  // textContent preserves the original line endings and never interprets
  // the telegram text as HTML.
  el.rawView.textContent = new TextDecoder("utf-8").decode(buffer);
  state.pendingRawSeq = null;
}

function handleServerError(message)
{
  if (message.request === "telegram")
  {
    // The last shown detail stays on screen: a failed refresh must never
    // blank the pane, only the state line reports the reason.
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
  // The realtime view sizes its poll cadence from the active read interval.
  state.lastStatus = status;

  let text = "State: " + (STATE_LABELS[status.state] || status.state);

  if (status.activeProfileId)
  {
    text += "  ·  profile " + status.activeProfileId;
  }

  if (status.receiverRunning)
  {
    // A test selection has no UART; live data always arrives on the fixed
    // P1 UART0 pins.
    text += status.testMode ? "  ·  test data (no UART)" : "  ·  live data (UART0 GPIO43/44)";
  }

  if (status.intervalSeconds)
  {
    text += "  ·  read every " + status.intervalSeconds + " s";
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

  // The header carries the program version and the device wall clock from
  // the status push; the clock stays empty until SNTP has set the time.
  if (status.firmwareVersion)
  {
    el.headerVersion.textContent = status.firmwareVersion;
  }

  el.headerClock.textContent = status.deviceTime || "no time yet";
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

//-- The two data-source selections as published by the device, with the
//-- firmware defaults as a fallback before the first config message.
function sourceChoices()
{
  const choices = (state.config !== null && state.config.choices) ? state.config.choices : {};

  return {
    liveGpio: (choices.liveGpio === undefined) ? 44 : choices.liveGpio,
    testGpio: (choices.testGpio === undefined) ? -1 : choices.testGpio,
  };
}

//-- Highlights the toggle end matching the current switch position.
function updateSourceLabels()
{
  const isTest = el.srcToggle.checked;
  el.srcLiveLabel.classList.toggle("active", !isTest);
  el.srcTestLabel.classList.toggle("active", isTest);
}

//-- Highlights the polarity end matching the RX polarity switch.
function updatePolarityLabels()
{
  const isInverted = el.rxPolarityToggle.checked;
  el.polNormalLabel.classList.toggle("active", !isInverted);
  el.polInvertedLabel.classList.toggle("active", isInverted);
}

//-- Highlights the polarity end matching the TX (Forwarding Data) switch.
function updateTxPolarityLabels()
{
  const isInverted = el.txPolarityToggle.checked;
  el.txPolNormalLabel.classList.toggle("active", !isInverted);
  el.txPolInvertedLabel.classList.toggle("active", isInverted);
}

//-- Highlights the orientation end matching the display rotation switch.
function updateRotationLabels()
{
  const isFlipped = el.displayRotationToggle.checked;
  el.rotNormalLabel.classList.toggle("active", !isFlipped);
  el.rotUpsideDownLabel.classList.toggle("active", isFlipped);
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

  // The data source is a two-position toggle: live data on the fixed P1
  // UART0 pins (GPIO43/44) or the internal test generator.
  const source = sourceChoices();
  el.srcToggle.checked = !!(settings && (settings.rxGpio === source.testGpio));
  updateSourceLabels();

  // The P1 RX polarity switch maps to the stored rxInverted flag: normal
  // reads the idle-high signal directly, inverted flips the receive line.
  el.rxPolarityToggle.checked = !!(settings && settings.rxInverted);
  updatePolarityLabels();

  // The P1 Tx polarity (Forwarding Data) switch maps to the stored
  // txInverted flag: both copies of a burst go out at this polarity.
  el.txPolarityToggle.checked = !!(settings && settings.txInverted);
  updateTxPolarityLabels();

  // The display rotation switch maps to the stored displayFlipped flag:
  // normal is the assembled landscape, upside down turns it 180 degrees.
  el.displayRotationToggle.checked = !!(settings && settings.displayFlipped);
  updateRotationLabels();

  el.inpInterval.value = (settings && settings.intervalSeconds) ? String(settings.intervalSeconds) : "10";

  fillSelect(el.selCountry, choices.countries);

  const mbus = mbusOptions(choices.mbusTypes || []);

  [el.selMbus1, el.selMbus2, el.selMbus3, el.selMbus4].forEach((select) =>
  {
    fillSelect(select, mbus.values, mbus.labels);
  });

  if (settings)
  {
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

  // The switch position maps to the stored RX selection: the fixed live
  // UART0 pin or the test generator marker.
  const source = sourceChoices();
  const rxGpio = el.srcToggle.checked ? source.testGpio : source.liveGpio;

  // The interval is validated by clamping: below the minimum becomes the
  // minimum, above the maximum becomes the maximum, and the field shows the
  // value that will actually be applied.
  let interval = parseInt(el.inpInterval.value, 10);

  if (!Number.isFinite(interval) || (interval < 2))
  {
    interval = 2;
  }

  if (interval > 600)
  {
    interval = 600;
  }

  el.inpInterval.value = String(interval);

  // The polarity switch position maps to the stored rxInverted flag; it only
  // affects live reception, the test generator opens no UART.
  const rxInverted = el.rxPolarityToggle.checked ? "1" : "0";

  // The Tx polarity switch position maps to the stored txInverted flag; it
  // governs both copies of every UART0 forwarding burst.
  const txInverted = el.txPolarityToggle.checked ? "1" : "0";

  // The rotation switch position maps to the stored displayFlipped flag.
  const displayFlipped = el.displayRotationToggle.checked ? "1" : "0";

  const body = "cmd=save" +
               "&rxGpio=" + encodeURIComponent(rxGpio) +
               "&rxInverted=" + rxInverted +
               "&txInverted=" + txInverted +
               "&displayFlipped=" + displayFlipped +
               "&interval=" + encodeURIComponent(interval) +
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

// The log is a plain scrolling window over the retained records: rows are
// not clickable and carry no selection. Each row shows the meter time of
// OBIS 0-0:1.0.0 (for example 17:37:38) instead of a receive uptime. The
// detail pane is filled once automatically and then stays frozen on that
// record: new telegrams refresh the log rows only. Show starts a live
// follow (the button becomes Pause): every newly accepted telegram is
// shown until Pause freezes the shown record again.
function renderTelegramList(telegrams)
{
  el.telegramLog.textContent = "";

  if (telegrams.length === 0)
  {
    // Only a never-filled pane is hidden: a shown detail survives an empty
    // list so the decode summary and raw telegram never blink away.
    if (state.selectedSeq === null)
    {
      el.telegramDetail.hidden = true;
      setTelegramState("No telegram accepted yet.");
    }
    return;
  }

  telegrams.forEach((telegram) =>
  {
    const line = document.createElement("div");
    line.className = "logLine";
    const crc = telegram.crcVerified ? "crc ok" : "no crc";
    const meterTime = telegram.meterTimeText || "--:--:--";
    line.textContent = "#" + telegram.sequence + "  " + meterTime + "  " + telegram.profileId +
                       "  v" + telegram.reportedVersion + "  " + crc;
    el.telegramLog.appendChild(line);
  });

  state.latestSeq = telegrams[0].sequence;

  // While Show is running every newly accepted telegram replaces the shown
  // record in the decode summary and raw view; after Pause the pane stays
  // frozen on the last shown record. Without any selection yet the pane is
  // filled once so it is never empty.
  if (state.followLatest && (telegrams[0].sequence !== state.selectedSeq))
  {
    selectTelegram(telegrams[0].sequence);
  }
  else if (state.selectedSeq === null)
  {
    selectTelegram(state.latestSeq);
  }
}

function selectTelegram(sequence)
{
  state.selectedSeq = sequence;
  el.telegramDetail.hidden = false;
  wsSend("cmd=telegram&seq=" + encodeURIComponent(sequence));
  wsSend("cmd=raw&seq=" + encodeURIComponent(sequence));
}

function renderDetail(detail)
{
  const meta = detail.meta;

  // A message without a usable meta must never blank the shown record:
  // keep the previous summary and raw view untouched instead.
  if (!meta || (typeof meta.sequence !== "number"))
  {
    return;
  }

  el.recordMeta.textContent = "#" + meta.sequence + " · meter time " + (meta.meterTimeText || "unavailable") +
                              " · captured " + formatUptime(meta.receivedUptimeMs) +
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
//-- Realtime power view
//-- --------------------------------------------------------------------------

// Replica of the TFT power panel: one centre-zero bar per phase plus Totaal
// on the same ladder the display uses (the device sends the picked scale,
// so both present the same numbers), with the measured watt value above
// each bar. Consumption grows red to the left of centre, feed-in green to
// the right. Below the bars a vertical-bar history follows the total power
// across the last 90 accepted measurements kept by the device: one
// centre-zero bar per accepted telegram on hairline scale division lines.
const POWER_ROWS =
[
  {label: "Fase 1"},
  {label: "Fase 2"},
  {label: "Fase 3"},
  {label: "Totaal"},
];

// The device keeps and sends the newest 90 accepted total watts for the
// bar history (one sample per accepted telegram, oldest first); this cap
// also guards the local fallback of older firmware without the array.
const POWER_HISTORY_MAX = 90;

const powerRowRefs = [];
let powerTimer = null;

// Scale ladder step of the newest power frame: kept for redraws (resizes)
// that happen without a fresh message.
let lastPowerScale = 500;

function buildPowerRows()
{
  el.powerRows.textContent = "";
  powerRowRefs.length = 0;

  POWER_ROWS.forEach((row) =>
  {
    const rowDiv = document.createElement("div");
    rowDiv.className = "powerRow";

    const head = document.createElement("div");
    head.className = "powerRowHead";

    const label = document.createElement("span");
    label.className = "powerPhaseLabel";
    label.textContent = row.label;

    const chips = document.createElement("span");
    chips.className = "powerChips";

    const ampsChip = document.createElement("span");
    ampsChip.className = "chip";

    const voltsChip = document.createElement("span");
    voltsChip.className = "chip";

    chips.appendChild(ampsChip);
    chips.appendChild(voltsChip);
    head.appendChild(label);
    head.appendChild(chips);

    const track = document.createElement("div");
    track.className = "powerBarTrack";

    const consumeFill = document.createElement("div");
    consumeFill.className = "powerBarFill consume";

    const produceFill = document.createElement("div");
    produceFill.className = "powerBarFill produce";

    const zeroLine = document.createElement("div");
    zeroLine.className = "powerBarZero";

    const tickLeft = document.createElement("span");
    tickLeft.className = "powerTick left";

    const tickMid = document.createElement("span");
    tickMid.className = "powerTick mid";
    tickMid.textContent = "0";

    const tickRight = document.createElement("span");
    tickRight.className = "powerTick right";

    track.appendChild(consumeFill);
    track.appendChild(produceFill);
    track.appendChild(zeroLine);
    track.appendChild(tickLeft);
    track.appendChild(tickMid);
    track.appendChild(tickRight);

    const value = document.createElement("div");
    value.className = "powerWatt";

    rowDiv.appendChild(head);
    rowDiv.appendChild(value);
    rowDiv.appendChild(track);
    el.powerRows.appendChild(rowDiv);

    powerRowRefs.push({label: label, ampsChip: ampsChip, voltsChip: voltsChip, consumeFill: consumeFill,
                       produceFill: produceFill, tickLeft: tickLeft, tickRight: tickRight, value: value});
  });
}

function formatWatt(watts)
{
  const rounded = Math.round(watts);
  const sign = (rounded < 0) ? "-" : ((rounded > 0) ? "+" : "");
  return sign + Math.abs(rounded).toLocaleString("en-GB") + " W";
}

function renderPower(message)
{
  if (state.screen !== "power")
  {
    return;
  }

  if (!message.valid)
  {
    el.powerPanel.hidden = true;
    return;
  }

  el.powerPanel.hidden = false;

  if (powerRowRefs.length === 0)
  {
    buildPowerRows();
  }

  el.powerTelegram.textContent = message.testMode ? "test data" : "";
  el.powerScaleLabel.textContent = "Bar scale " + message.scale + " W · the same ladder the display uses";
  lastPowerScale = message.scale;

  const values = [message.phases[0], message.phases[1], message.phases[2], message.total];

  values.forEach((entry, index) =>
  {
    const row = powerRowRefs[index];
    const watts = (typeof entry.watts === "number") ? entry.watts : 0;
    const fraction = Math.min(1, Math.abs(watts) / message.scale);

    row.consumeFill.style.width = (watts < 0) ? (fraction * 50).toFixed(2) + "%" : "0%";
    row.produceFill.style.width = (watts > 0) ? (fraction * 50).toFixed(2) + "%" : "0%";

    row.tickLeft.textContent = "-" + message.scale;
    row.tickRight.textContent = String(message.scale);

    row.value.textContent = formatWatt(watts);
    row.value.classList.toggle("consume", watts < 0);
    row.value.classList.toggle("produce", watts > 0);

    const amps = (typeof entry.amps === "number") ? entry.amps : 0;
    const volts = (typeof entry.volts === "number") ? entry.volts : 0;
    row.ampsChip.textContent = (amps >= 0 ? "" : "-") + Math.abs(amps).toFixed(2) + " A";
    row.voltsChip.textContent = volts.toFixed(1) + " V";
    row.voltsChip.classList.toggle("dim", volts <= 0);
  });

  // The device keeps the last 90 accepted total watts and sends them with
  // every frame, oldest first: the history then always shows the last 90
  // accepted measurements, across reloads, reconnects and view switches.
  // Without the array (older firmware) fall back to sampling the pushed
  // frames locally: one sample per newly accepted telegram.
  if (Array.isArray(message.history))
  {
    state.powerHistory = message.history.slice(-POWER_HISTORY_MAX).map((total) =>
    {
      return {total: total};
    });
  }
  else if (message.sequence !== state.powerLastSeq)
  {
    state.powerLastSeq = message.sequence;
    state.powerHistory.push({seq: message.sequence, total: message.total.watts});

    while (state.powerHistory.length > POWER_HISTORY_MAX)
    {
      state.powerHistory.shift();
    }
  }

  drawPowerSpark(message.scale);
}

function cssVar(name, fallback)
{
  const value = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  return value || fallback;
}

function drawPowerSpark(scale)
{
  const canvas = el.powerSpark;

  if (canvas === null)
  {
    return;
  }

  const wrap = canvas.parentElement;
  const cssWidth = Math.max(240, wrap.clientWidth);
  const cssHeight = 130;
  const dpr = window.devicePixelRatio || 1;

  canvas.width = Math.round(cssWidth * dpr);
  canvas.height = Math.round(cssHeight * dpr);
  canvas.style.width = cssWidth + "px";
  canvas.style.height = cssHeight + "px";

  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cssWidth, cssHeight);

  const mid = cssHeight / 2;
  const padY = 10;
  const halfSpan = mid - padY;
  const consumeColor = "#ff3b30";
  const produceColor = "#34c759";
  const gridColor = cssVar("--separator", "rgba(60, 60, 67, 0.12)");
  const zeroColor = cssVar("--hairline", "rgba(60, 60, 67, 0.29)");
  const secondaryColor = cssVar("--secondary", "#6e6e73");

  // Very thin scale division lines every quarter of the ladder step, above
  // and below the zero line; the zero line itself stays a touch stronger.
  ctx.lineWidth = 0.5;

  [0.25, 0.5, 0.75, 1].forEach((fraction) =>
  {
    const offset = fraction * halfSpan;
    ctx.strokeStyle = gridColor;
    ctx.beginPath();
    ctx.moveTo(0, mid - offset);
    ctx.lineTo(cssWidth, mid - offset);
    ctx.moveTo(0, mid + offset);
    ctx.lineTo(cssWidth, mid + offset);
    ctx.stroke();
  });

  ctx.strokeStyle = zeroColor;
  ctx.beginPath();
  ctx.moveTo(0, mid);
  ctx.lineTo(cssWidth, mid);
  ctx.stroke();

  ctx.font = "10px " + cssVar("--mono", "monospace");
  ctx.fillStyle = secondaryColor;
  ctx.textBaseline = "top";
  ctx.fillText("+" + scale + " W", 4, padY - 8 < 0 ? 0 : padY - 8);
  ctx.textBaseline = "bottom";
  ctx.fillText("-" + scale + " W", 4, cssHeight);

  const samples = state.powerHistory;

  if (samples.length === 0)
  {
    return;
  }

  // One slot per retained sample so the bars keep a steady pitch as the
  // history fills up; the bar stays inside its slot.
  const slot = cssWidth / samples.length;
  const barWidth = Math.max(1, Math.min(10, Math.floor(slot * 0.65)));

  function yOf(total)
  {
    const clamped = Math.max(-scale, Math.min(scale, total));
    return mid - (clamped / scale) * halfSpan;
  }

  function centreOf(index)
  {
    return (samples.length === 1) ? (cssWidth / 2) : ((index + 0.5) * slot);
  }

  // Centre-zero bars: consumption hangs red below the zero line, feed-in
  // grows green above it, with a brighter cap at the value end so even the
  // thinnest bars stay readable.
  samples.forEach((sample, index) =>
  {
    const produce = sample.total >= 0;
    const barHeight = Math.max(1, Math.abs(yOf(sample.total) - mid));
    const x = centreOf(index) - barWidth / 2;

    ctx.fillStyle = produce ? "rgba(52, 199, 89, 0.7)" : "rgba(255, 59, 48, 0.7)";
    ctx.fillRect(x, produce ? (mid - barHeight) : mid, barWidth, barHeight);

    ctx.fillStyle = produce ? produceColor : consumeColor;
    ctx.fillRect(x, produce ? (mid - barHeight) : (mid + barHeight - 1), barWidth, 1);
  });

  // The latest reading as a glowing dot above the newest bar.
  const last = samples[samples.length - 1];
  const lastX = centreOf(samples.length - 1);
  const lastY = yOf(last.total);
  const lastColor = (last.total < 0) ? consumeColor : produceColor;

  ctx.beginPath();
  ctx.arc(lastX, lastY, 6, 0, Math.PI * 2);
  ctx.fillStyle = (last.total < 0) ? "rgba(255, 59, 48, 0.25)" : "rgba(52, 199, 89, 0.25)";
  ctx.fill();
  ctx.beginPath();
  ctx.arc(lastX, lastY, 3, 0, Math.PI * 2);
  ctx.fillStyle = lastColor;
  ctx.fill();
}

function powerPollDelayMs()
{
  const intervalSeconds = (state.lastStatus && state.lastStatus.intervalSeconds) ? state.lastStatus.intervalSeconds : 5;
  const delay = Math.min(5000, intervalSeconds * 1000);
  return Math.max(1000, delay);
}

// The realtime view refreshes at least every five seconds; with a shorter
// read interval it follows that cadence instead. Device pushes on every
// accepted telegram keep the bars fresh in between.
function updatePowerPolling()
{
  clearTimeout(powerTimer);
  powerTimer = null;

  if (state.screen !== "power")
  {
    return;
  }

  powerTimer = setTimeout(() =>
  {
    wsSend("cmd=power");
    updatePowerPolling();
  }, powerPollDelayMs());
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
  updatePowerPolling();
}

function boot()
{
  bindElements();
  buildTabBar();
  el.settingsForm.addEventListener("submit", saveSettings);

  el.srcToggle.addEventListener("change", updateSourceLabels);
  el.rxPolarityToggle.addEventListener("change", updatePolarityLabels);
  el.txPolarityToggle.addEventListener("change", updateTxPolarityLabels);
  el.displayRotationToggle.addEventListener("change", updateRotationLabels);

  // Show starts the live follow of every newly accepted telegram (the
  // button becomes Pause); Pause freezes the shown telegram again.
  el.showTelegram.addEventListener("click", () =>
  {
    state.followLatest = !state.followLatest;

    if (state.followLatest && (state.latestSeq !== null))
    {
      selectTelegram(state.latestSeq);
    }

    el.showTelegram.textContent = state.followLatest ? "Pause" : "Show";
  });

  el.selCountry.addEventListener("change", () =>
  {
    fillProfiles(el.selCountry.value, null);
  });

  // The bar history is drawn to the container width: redraw on resize while
  // the realtime view is open (before the first frame there is no scale to
  // draw with, so the last known one is guarded inside).
  window.addEventListener("resize", () =>
  {
    if (state.screen === "power")
    {
      drawPowerSpark(lastPowerScale);
    }
  });

  wsConnect();
  showScreen("telegrams");
}

document.addEventListener("DOMContentLoaded", boot);
