// lastDSMRlogger SPA
// Date: 2026-11-23 · Build: 35
//
// Views share one page and are listed in the VIEWS registry: the tab bar
// (an iOS-style segmented control) is built from it, so a new feature only
// needs one registry entry plus a matching section in index.html and a
// command on the device (the API view needs no device command: it fetches
// the published endpoint index over plain HTTP instead — see its refresh
// hook). Every registry entry also carries a bar group: the logger bar shows
// the feature views and ends with [System], the system bar shows the
// settings views (Settings, Show RingFile, Tariffs, API) and ends with
// [Back], which returns to the logger screen last shown. Every bar is named
// by a macOS-style leading caption ("Logger:" / "System:"). The realtime
// Phase Load view opens first (at every start and reload of the page).
// All dynamic data travels over one WebSocket at /ws: the
// device pushes status, the telegram list and the realtime power frame, the
// browser sends form-urlencoded commands (cmd=config, cmd=save,
// cmd=telegram, cmd=raw, cmd=view, cmd=telegrams, cmd=power,
// cmd=ringfile, cmd=graph&ring=hours|days|months, cmd=actualgraph). Raw telegram
// bytes arrive as a binary frame
// behind their JSON header. Every rendering path uses textContent so
// telegram text is never interpreted as HTML. The active screen name is
// reported to the device so the on-device debug lines show what the browser
// is displaying. If the link drops, the client reconnects and re-requests
// what the current view needs. Only one browser page holds the web control
// slot at a time (the telnet console has its own slot and never displaces a
// page): when another browser page claims control, the device sends a
// "takenover" message and closes this socket; the page then
// shows its takeover popup (no auto-reconnect, or control would bounce back
// on its own) and the Take back control button reloads the page so its
// first frame claims control again.

"use strict";

const state =
{
  // The realtime Phase Load view opens first (at every start and reload).
  screen: "power",
  // Which tab bar the header shows: "logger" (feature views + System) or
  // "system" (settings views + Back).
  bar: "logger",
  // The logger-group view shown before the system bar opened: the Back
  // button restores this screen.
  lastLoggerView: "power",
  config: null,
  selectedSeq: null,
  latestSeq: null,
  // Live follow is on from the start: the newest accepted telegram is shown
  // until the user freezes the pane with Pause (the button then reads Show).
  followLatest: true,
  pendingRawSeq: null,
  ws: null,
  wsReady: false,
  // True once the device reports that another browser page took control: the
  // popup is open and the auto-reconnect loop must stay halted until the
  // user presses Take back control (which reloads the page).
  controlHalted: false,
  // Anti ping-pong guard: a session that is closed again within seconds of
  // opening (three times in a row) means control keeps moving to a browser
  // page that never halts — typically a stale page running an app.js older than
  // the takeover popup, which reconnects and re-claims forever. The page
  // then stops retrying and hands the decision to the user through the
  // takeover popup. A moment of real traffic resets the streak.
  wsOpenedAt: 0,
  rapidCloseStreak: 0,
  lastStatus: null,
  powerLastSeq: null,
  powerHistory: [],
  // The MQTT password never travels from the device (security rule): the
  // field is never prefilled and stays empty until the operator types a new
  // password. Only then does a save carry the mqttPassword field at all;
  // while untouched, the device keeps the stored password.
  mqttPasswordChanged: false,
};

const WS_RAPID_CLOSE_MS = 3000;
const WS_RAPID_CLOSE_LIMIT = 3;

// One entry per feature view: the tab bar and the view switching are driven
// by this registry. To add a feature: add an entry here, add a matching
// <section class="card" hidden id="viewXxx"> to index.html and answer the
// view's command on the device. The group property assigns the view to one
// of the two bars: the logger bar ("logger", ended by the [System] button)
// and the system bar ("system", ended by the [Back] button).
const VIEWS =
[
  {id: "settings", label: "Settings", section: "viewSettings", command: "cmd=config",
   group: "system"},
  {id: "telegrams", label: "Telegrams", section: "viewTelegrams", command: "cmd=telegrams",
   group: "logger"},
  {id: "power", label: "Phase Load", section: "viewPower", command: "cmd=power",
   group: "logger"},
  {id: "ringfile", label: "Show RingFile", section: "viewRingFile",
   command: () => "cmd=ringfile&ring=" + selectedRing(), group: "system"},
  {id: "hoursGraph", label: "Hours", section: "viewHoursGraph",
   command: "cmd=graph&ring=hours", group: "logger"},
  {id: "daysGraph", label: "Days", section: "viewDaysGraph",
   command: "cmd=graph&ring=days", group: "logger"},
  {id: "monthsGraph", label: "Months", section: "viewMonthsGraph",
   command: "cmd=graph&ring=months", group: "logger"},
  {id: "actualGraph", label: "Actual", section: "viewActualGraph",
   command: "cmd=actualgraph", group: "logger"},
  // The Tariffs view reads the stored schedule over plain HTTP (see
  // refreshTariffs), so it carries a refresh hook like the API view
  // instead of a WebSocket command.
  {id: "tariffs", label: "Tariffs", section: "viewTariffs", command: null,
   refresh: refreshTariffs, group: "system"},
  // The API view needs no WebSocket command: it fetches the device's own
  // endpoint index from GET /api/v3 (see refreshApiList), so it carries a
  // refresh hook instead of a command.
  {id: "api", label: "API", section: "viewApi", command: null,
   refresh: refreshApiList, group: "system"},
];

// Resolves a view's command: either a fixed string or a function evaluated
// when the view is (re)requested, so the ring file view always asks for the
// ring its radio group has selected.
function viewCommand(view)
{
  return (typeof view.command === "function") ? view.command() : view.command;
}

const el = {};

// Filled by buildTabBar() for the active bar; maps a view id to its tab
// button (only the buttons of the shown bar are in the map).
const tabButtons = {};

function bindElements()
{
  const ids = ["tabBar", "tabBarCaption", "statusStrip", "settingsMessage", "settingsForm",
               "headerVersion", "headerClock",
               "srcToggle", "srcLiveLabel", "srcTestLabel",
               "rxPolarityToggle", "polNormalLabel", "polInvertedLabel",
               "txPolarityToggle", "txPolNormalLabel", "txPolInvertedLabel", "inpInterval",
               "displayRotationToggle", "rotNormalLabel", "rotUpsideDownLabel",
               "selCountry", "selProfile", "selMbus1", "selMbus2", "selMbus3", "selMbus4",
               "inpMqttBroker", "inpMqttPort", "inpMqttUser", "inpMqttPassword", "inpMqttTopic",
               "inpMqttClientId", "inpMqttSubTopic",
               "inpMqttInterval", "inpHostname", "inpFixedIp",
               "inpRfsSync", "rfsWearEstimate", "inpDynamicTariffs",
               "inpTariff1", "inpTariff2", "inpTariff1Export", "inpTariff2Export",
               "inpTariffGas", "inpNetworkCosts",
               "saveSettings", "telegramState", "showTelegram", "telegramLog",
               "telegramDetail", "recordMeta", "recordNotes", "summaryTable", "rawView", "footerInfo",
               "powerPanel", "powerTelegram", "powerRows",
               "powerScaleLabel", "powerSpark",
               "ringFileState", "refreshRingFile", "ringFileTable", "ringFileKeyHeader",
               "hoursGraphState", "refreshHoursGraph", "hoursGraphCanvas",
               "hoursGraphCanvasWrap", "hoursGraphTableWrap", "hoursGraphTable",
               "daysGraphState", "refreshDaysGraph", "daysGraphCanvas",
               "daysGraphCanvasWrap", "daysGraphTableWrap", "daysGraphTable",
               "monthsGraphState", "refreshMonthsGraph", "monthsGraphCanvas",
               "monthsGraphCanvasWrap", "monthsGraphTableWrap", "monthsGraphTable",
               "monthsGraphCostsTableWrap", "monthsGraphCostsTable",
               "actualGraphState", "refreshActualGraph", "actualGraphCanvas", "actualGraphChecks",
               "tariffsState", "refreshTariffs", "tariffsTable",
               "viewSettings", "viewTelegrams", "viewPower", "viewRingFile", "viewHoursGraph",
               "viewDaysGraph", "viewMonthsGraph", "viewActualGraph", "viewTariffs", "viewApi",
               "apiState", "apiList",
               "takeoverModal", "takeoverMessage", "takeoverTakeBack"];

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
    if (view.command !== null)
    {
      wsSend(viewCommand(view));
    }
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
    state.wsOpenedAt = Date.now();
    el.statusStrip.textContent = "Connected to the logger.";
    requestCurrentView();
    updatePowerPolling();
  };

  socket.onclose = () =>
  {
    state.wsReady = false;
    state.ws = null;

    // Control was taken over (or the socket was closed right after the
    // takeover message): the popup owns the screen now and auto-reconnect
    // would silently steal control back, so it stays halted.
    if (state.controlHalted)
    {
      el.statusStrip.textContent = "Control was taken over by another browser page.";
      return;
    }

    // Ping-pong guard: closed again seconds after opening means another
    // browser page keeps claiming control without ever halting. After three such
    // quick cycles the page stops fighting and shows the popup instead of
    // reconnecting forever.
    const livedFor = state.wsOpenedAt ? Date.now() - state.wsOpenedAt : Number.MAX_SAFE_INTEGER;

    if (livedFor < WS_RAPID_CLOSE_MS)
    {
      state.rapidCloseStreak++;
    }
    else
    {
      state.rapidCloseStreak = 0;
    }

    if (state.rapidCloseStreak >= WS_RAPID_CLOSE_LIMIT)
    {
      showTakeoverModal("Another browser page keeps taking control of the logger. Press the " +
                        "button to reconnect and take control again.");
      return;
    }

    el.statusStrip.textContent = "Connection to the logger lost: reconnecting...";
    setTimeout(() =>
    {
      if (!state.controlHalted)
      {
        wsConnect();
      }
    }, 2000);
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
    // Real traffic means the link is healthy: forget earlier quick closes.
    // Only a session that survived the rapid-close window counts as healthy;
    // the periodic status push arrives within the first second, so resetting
    // on every message would let two fighting tabs ping-pong forever.
    if (state.wsOpenedAt && (Date.now() - state.wsOpenedAt) >= WS_RAPID_CLOSE_MS)
    {
      state.rapidCloseStreak = 0;
    }

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

    case "ringfile":
      renderRingFileList(message);
      break;

    case "graph":
      handleGraphMessage(message);
      break;

    case "hoursgraph":
      // Legacy frame (the old cmd=hoursgraph spelling answers under the
      // original type name): route it to the hours view.
      handleGraphMessage({ring: message.ring || "hours", records: message.records,
                          available: message.available});
      break;

    case "actualgraph":
      renderActualGraph(message);
      break;

    case "raw":
      state.pendingRawSeq = message.sequence;
      break;

    case "save":
      handleSaveResult(message);
      break;

    case "takenover":
      showTakeoverModal(message.message);
      break;

    case "error":
      handleServerError(message);
      break;

    default:
      break;
  }
}

//-- Control-takeover popup (markup at the end of index.html). The device
//-- sends this message just before it closes the socket of a page that lost
//-- the web control slot to another browser page.
//-- Auto-reconnect stays halted while the popup is open; Take back control
//-- reloads the page, and the fresh page's first frame claims control back.
function showTakeoverModal(message)
{
  state.controlHalted = true;
  state.wsReady = false;

  if (typeof message === "string" && message.length > 0)
  {
    el.takeoverMessage.textContent = message;
  }

  el.statusStrip.textContent = "Control was taken over by another browser page.";
  el.takeoverModal.hidden = false;
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
  else if (message.request === "ringfile")
  {
    setRingFileState(message.error || "Ring file unavailable.");
  }
  else if (message.request === "hoursgraph")
  {
    setPeriodGraphState("hoursGraph", message.error || "Hours graph unavailable.");
  }
  else if (message.request === "graph")
  {
    // The frame echoes the ring when the device knew it; without it (the
    // error came before the selector was parsed) report on the open graph
    // view, since only that one requests series.
    const viewId = graphViewByRing(message.ring) ||
                   ((state.screen in PERIOD_GRAPHS) ? state.screen : null);

    if (viewId !== null)
    {
      setPeriodGraphState(viewId, message.error || "Graph unavailable.");
    }
  }
  else if (message.request === "actualgraph")
  {
    setActualGraphState(message.error || "Actual graph unavailable.");
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

//-- Shows only the fields of one settings section: the radio bar on top of
//-- the settings screen picks which .settingsGroup div of index.html stays
//-- on screen (p1port, display, mbus, mqtt, network, store or tariffs — the
//-- same groups the telnet settings ini writes). Hidden fields keep their
//-- values and Save always submits the complete configuration (see
//-- saveSettings), so the picker only changes what is visible.
function showSettingsGroup(group)
{
  document.querySelectorAll(".settingsGroup").forEach((section) =>
  {
    section.hidden = (section.dataset.group !== group);
  });
}

// The bounds of the rfs sync interval, taken from the published choices
// with the documented fallback when the frame carries none.
function rfsSyncBounds()
{
  const choices = (state.config && state.config.choices) ? state.config.choices : {};

  return {
    min: Number.isFinite(choices.rfsSyncMin) ? choices.rfsSyncMin : 1,
    max: Number.isFinite(choices.rfsSyncMax) ? choices.rfsSyncMax : 60
  };
}

// Clamps the entered rfs sync interval to the published bounds, writes the
// accepted value back into the field and refreshes the wear estimate.
function clampRfsSync()
{
  const bounds = rfsSyncBounds();
  let minutes = parseInt(el.inpRfsSync.value, 10);

  if (!Number.isFinite(minutes) || (minutes < bounds.min))
  {
    minutes = bounds.min;
  }

  if (minutes > bounds.max)
  {
    minutes = bounds.max;
  }

  el.inpRfsSync.value = String(minutes);
  return minutes;
}

// Rough LittleFS lifetime estimate behind the sync interval field: one
// synchronisation is estimated (from the device-published inputs) to cause
// blockWritesPerSync block-sized writes, spread uniformly over the volume's
// blocks, against the flash program/erase cycle rating. A deliberately
// pessimistic figure, worded as the estimate it is.
function updateRfsWearEstimate()
{
  if (!el.rfsWearEstimate)
  {
    return;
  }

  const choices = (state.config && state.config.choices) ? state.config.choices : {};
  const wear = choices.rfsWear;

  if (!wear || !Number.isFinite(wear.totalBlocks) || (wear.totalBlocks <= 0) ||
      !Number.isFinite(wear.blockWritesPerSync) || !Number.isFinite(wear.flashWriteCycles))
  {
    el.rfsWearEstimate.textContent = "";
    return;
  }

  const minutes = clampRfsSync();
  const syncsPerYear = (365 * 24 * 60) / minutes;
  const cyclesPerBlockPerYear = (syncsPerYear * wear.blockWritesPerSync) / wear.totalBlocks;
  const years = wear.flashWriteCycles / cyclesPerBlockPerYear;

  if (!Number.isFinite(years) || (years <= 0))
  {
    el.rfsWearEstimate.textContent = "";
    return;
  }

  const shown = (years >= 10) ? String(Math.round(years)) : years.toFixed(1);
  el.rfsWearEstimate.textContent = "~ " + shown + " year LittleFS wear estimate";
}

// Clamps one manual tariff price to the device bounds and writes the
// accepted value back into the field.
function clampTariff(input)
{
  let value = parseFloat(input.value);

  if (!Number.isFinite(value))
  {
    value = 0;
  }

  if (value < -5)
  {
    value = -5;
  }

  if (value > 5)
  {
    value = 5;
  }

  input.value = String(value);
  return value;
}

// Clamps the fixed monthly network and transport amount to the device
// bounds (0 .. 1000 EUR) and writes the accepted value back into the
// field.
function clampNetworkCosts(input)
{
  let value = parseFloat(input.value);

  if (!Number.isFinite(value))
  {
    value = 0;
  }

  if (value < 0)
  {
    value = 0;
  }

  if (value > 1000)
  {
    value = 1000;
  }

  input.value = String(value);
  return value;
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

  // MQTT publication and network identity prefill straight from storage;
  // an absent broker simply keeps publication off.
  el.inpMqttBroker.value = (settings && settings.mqttBroker) ? settings.mqttBroker : "";
  el.inpMqttPort.value = (settings && settings.mqttPort) ? String(settings.mqttPort) : "1883";
  el.inpMqttUser.value = (settings && settings.mqttUser) ? settings.mqttUser : "";
  // The settings frame never carries the password, so the field is always
  // cleared on (re)fill: it shows nothing stored and only travels back when
  // the operator types a new password (see state.mqttPasswordChanged).
  el.inpMqttPassword.value = "";
  state.mqttPasswordChanged = false;
  el.inpMqttTopic.value = (settings && settings.mqttTopTopic) ? settings.mqttTopTopic : "";
  // The tariff topic is <top topic>/<client id>/<sub topic>: an empty
  // client id leaves the device segment out and an empty sub topic means
  // newTariffs (see loggerTariffsEffectiveTopic on the device).
  el.inpMqttClientId.value = (settings && settings.mqttClientId) ? settings.mqttClientId : "";
  el.inpMqttSubTopic.value = (settings && settings.mqttSubTopic) ? settings.mqttSubTopic : "";
  el.inpMqttInterval.value =
      (settings && settings.mqttIntervalSeconds) ? String(settings.mqttIntervalSeconds) : "0";
  el.inpHostname.value = (settings && settings.hostname) ? settings.hostname : "";
  el.inpFixedIp.value = (settings && settings.fixedIpLastOctet) ? String(settings.fixedIpLastOctet) : "0";

  // Meter store and tariffs prefill from storage: the sync interval, the
  // manual tariff prices (the import and export price of both energy
  // tariffs and the gas price) and the dynamic tariffs switch.
  el.inpRfsSync.value =
      (settings && settings.rfsSyncIntervalMinutes) ? String(settings.rfsSyncIntervalMinutes) : "10";
  el.inpTariff1.value =
      (settings && Number.isFinite(settings.tariff1ImportPricePerKwh)) ? String(settings.tariff1ImportPricePerKwh) : "0";
  el.inpTariff2.value =
      (settings && Number.isFinite(settings.tariff2ImportPricePerKwh)) ? String(settings.tariff2ImportPricePerKwh) : "0";
  el.inpTariff1Export.value =
      (settings && Number.isFinite(settings.tariff1ExportPricePerKwh)) ? String(settings.tariff1ExportPricePerKwh) : "0";
  el.inpTariff2Export.value =
      (settings && Number.isFinite(settings.tariff2ExportPricePerKwh)) ? String(settings.tariff2ExportPricePerKwh) : "0";
  el.inpTariffGas.value =
      (settings && Number.isFinite(settings.tariffGasPricePerM3)) ? String(settings.tariffGasPricePerM3) : "0";
  el.inpNetworkCosts.value =
      (settings && Number.isFinite(settings.networkCostsPerMonthEuro)) ? String(settings.networkCostsPerMonthEuro) : "0";
  el.inpDynamicTariffs.checked = !!(settings && settings.dynamicTariffsEnabled);
  updateRfsWearEstimate();

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

  // The broker port is clamped like the interval: an unusable value becomes
  // the default 1883 whenever a broker is named at all.
  let mqttPort = parseInt(el.inpMqttPort.value, 10);
  const mqttBroker = (el.inpMqttBroker.value || "").trim();

  if (!Number.isFinite(mqttPort) || (mqttPort < 1) || (mqttPort > 65535))
  {
    mqttPort = (mqttBroker.length > 0) ? 1883 : 0;
  }

  el.inpMqttPort.value = String(mqttPort);

  // The broadcast interval is clamped to 0..86400; 0 keeps publication off.
  let mqttInterval = parseInt(el.inpMqttInterval.value, 10);

  if (!Number.isFinite(mqttInterval) || (mqttInterval < 0))
  {
    mqttInterval = 0;
  }

  if (mqttInterval > 86400)
  {
    mqttInterval = 86400;
  }

  el.inpMqttInterval.value = String(mqttInterval);

  // The hostname is stored lowercase; the fixed IP keeps only a valid last
  // octet (anything unusable means plain DHCP).
  const hostname = (el.inpHostname.value || "").trim().toLowerCase();
  el.inpHostname.value = hostname;

  let fixedIp = parseInt(el.inpFixedIp.value, 10);

  if (!Number.isFinite(fixedIp) || (fixedIp < 1))
  {
    fixedIp = 0;
  }

  if (fixedIp > 254)
  {
    fixedIp = 254;
  }

  el.inpFixedIp.value = String(fixedIp);

  // The rfs sync interval and the manual tariff prices (the import and
  // export price of both energy tariffs and the gas price) are clamped to
  // the published bounds; the wear estimate follows the value that will be
  // applied. The switch position maps to the stored dynamicTariffsEnabled
  // flag.
  const rfsSyncMinutes = clampRfsSync();
  updateRfsWearEstimate();
  const tariff1 = clampTariff(el.inpTariff1);
  const tariff2 = clampTariff(el.inpTariff2);
  const tariff1Export = clampTariff(el.inpTariff1Export);
  const tariff2Export = clampTariff(el.inpTariff2Export);
  const tariffGas = clampTariff(el.inpTariffGas);
  const networkCosts = clampNetworkCosts(el.inpNetworkCosts);
  const dynamicTariffs = el.inpDynamicTariffs.checked ? "1" : "0";

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
               "&mbus4=" + encodeURIComponent(el.selMbus4.value) +
               "&mqttBroker=" + encodeURIComponent(mqttBroker) +
               "&mqttPort=" + encodeURIComponent(mqttPort) +
               "&mqttUser=" + encodeURIComponent((el.inpMqttUser.value || "").trim()) +
               // The password field only travels when the operator typed a
               // new one: an absent field keeps the stored password (an
               // explicitly emptied field after typing clears it).
               (state.mqttPasswordChanged
                   ? "&mqttPassword=" + encodeURIComponent(el.inpMqttPassword.value || "")
                   : "") +
               "&mqttTopTopic=" + encodeURIComponent((el.inpMqttTopic.value || "").trim()) +
               "&mqttClientId=" + encodeURIComponent((el.inpMqttClientId.value || "").trim()) +
               "&mqttSubTopic=" + encodeURIComponent((el.inpMqttSubTopic.value || "").trim()) +
               "&mqttInterval=" + encodeURIComponent(mqttInterval) +
               "&hostname=" + encodeURIComponent(hostname) +
               "&fixedIp=" + encodeURIComponent(fixedIp) +
               "&rfsSyncMinutes=" + encodeURIComponent(rfsSyncMinutes) +
               "&tariff1=" + encodeURIComponent(tariff1) +
               "&tariff2=" + encodeURIComponent(tariff2) +
               "&tariff1Export=" + encodeURIComponent(tariff1Export) +
               "&tariff2Export=" + encodeURIComponent(tariff2Export) +
               "&tariffGas=" + encodeURIComponent(tariffGas) +
               "&networkCosts=" + encodeURIComponent(networkCosts) +
               "&dynamicTariffs=" + dynamicTariffs;

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

// The device stores field timestamps as ISO 8601 (2026-11-18T12:15:07); the
// summary table shows them day first instead: 18-11-2026 - 12:15:07. Text
// that does not match the ISO shape is returned unchanged.
function formatFieldTimestamp(text)
{
  const iso = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}:\d{2}:\d{2})/.exec(text || "");

  if (!iso)
  {
    return text || "";
  }

  return iso[3] + "-" + iso[2] + "-" + iso[1] + " - " + iso[4];
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
      field.name,
      // An empty value string marks an unavailable reading; a measured zero
      // keeps its own text and stays distinguishable.
      (field.value === "") ? "(unavailable)" : field.value,
      field.unit,
      field.hasTimestamp ? formatFieldTimestamp(field.timestamp) : "",
      (field.mbusChannel > 0) ? ("ch " + field.mbusChannel) : "",
    ];

    cells.forEach((text, index) =>
    {
      const cell = document.createElement("td");
      cell.textContent = text;

      // The value column (index 2): measured numbers align right so the
      // digits line up, text values and unavailable readings align left.
      if (index === 2)
      {
        cell.className = (field.value !== "" && !field.valueIsText) ? "valueNumeric" : "valueText";
      }

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
      const cells = ["0-" + channel + ":24.2.1", "mbusRdng", "(unavailable)", "", "", "ch " + channel];

      cells.forEach((text, index) =>
      {
        const cell = document.createElement("td");
        cell.textContent = text;

        if (index === 2)
        {
          cell.className = "valueText";
        }

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

// Replica of the TFT power panel: one centre-zero bar per phase plus Total
// on the same ladder the display uses (the device sends the picked scale,
// so both present the same numbers). One head line above each bar carries
// the phase name left, the measured watt centred (the only coloured text:
// red consumption, green feed-in) and the ampere and volt chips right.
// Below the bars a vertical-bar history follows the total power
// across the last 100 accepted measurements kept by the device: one
// centre-zero bar per accepted telegram on hairline scale division lines.
const POWER_ROWS =
[
  {label: "Phase 1"},
  {label: "Phase 2"},
  {label: "Phase 3"},
  {label: "Total"},
];

// The device keeps and sends the newest 100 accepted total watts for the
// bar history (one sample per accepted telegram, oldest first); this cap
// also guards the local fallback of older firmware without the array.
const POWER_HISTORY_MAX = 100;

const powerRowRefs = [];
let powerTimer = null;

// Hover popup of the total power history: with the mouse over the plot the
// bar under the cursor is labelled with its meter time and its total watts.
// powerSparkLayout keeps the drawing geometry of the newest frame,
// powerHoverX the x position of the mouse inside the canvas (null: outside).
let powerSparkLayout = null;
let powerHoverX = null;
let powerTip = null;

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

    // One head line per bar: phase name left, measured watt centred and
    // the ampere and volt chips right (only the watt is coloured).
    const value = document.createElement("div");
    value.className = "powerWatt";

    const label = document.createElement("span");
    label.className = "powerPhaseLabel";
    label.textContent = row.label;

    const watts = document.createElement("span");
    watts.className = "powerWattValue";

    const chips = document.createElement("span");
    chips.className = "powerChips";

    const ampsChip = document.createElement("span");
    ampsChip.className = "chip";

    const voltsChip = document.createElement("span");
    voltsChip.className = "chip";

    chips.appendChild(ampsChip);
    chips.appendChild(voltsChip);
    value.appendChild(label);
    value.appendChild(watts);
    value.appendChild(chips);

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

    rowDiv.appendChild(value);
    rowDiv.appendChild(track);
    el.powerRows.appendChild(rowDiv);

    powerRowRefs.push({label: label, watts: watts, ampsChip: ampsChip, voltsChip: voltsChip,
                       consumeFill: consumeFill, produceFill: produceFill, tickLeft: tickLeft,
                       tickRight: tickRight});
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

    // Only the watt text takes the red/green colouring; the phase label and
    // the chips keep their own fixed styling.
    row.watts.textContent = formatWatt(watts);
    row.watts.classList.toggle("consume", watts < 0);
    row.watts.classList.toggle("produce", watts > 0);

    const amps = (typeof entry.amps === "number") ? entry.amps : 0;
    const volts = (typeof entry.volts === "number") ? entry.volts : 0;
    row.ampsChip.textContent = (amps >= 0 ? "" : "-") + Math.abs(amps).toFixed(2) + " A";
    row.voltsChip.textContent = volts.toFixed(1) + " V";
    row.voltsChip.classList.toggle("dim", volts <= 0);
  });

  // The device keeps the last 100 accepted total watts and sends them with
  // every frame, oldest first: the history then always shows the last 100
  // accepted measurements, across reloads, reconnects and view switches.
  // Without the array (older firmware) fall back to sampling the pushed
  // frames locally: one sample per newly accepted telegram.
  if (Array.isArray(message.history))
  {
    state.powerHistory = message.history.slice(-POWER_HISTORY_MAX).map((entry) =>
    {
      // Current firmware sends {watts, time} objects; a plain number from
      // older firmware still renders (the tooltip then shows a dash).
      return (typeof entry === "number") ? {total: entry} : {total: entry.watts, time: entry.time};
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

  // Ladder labels follow the bar sides: consumption (the signed-negative
  // watts) grows up so its scale reads at the top, feed-in hangs down with
  // its scale at the bottom, like the used/returned panels of the graphs.
  ctx.font = "10px " + cssVar("--mono", "monospace");
  ctx.fillStyle = secondaryColor;
  ctx.textBaseline = "top";
  ctx.fillText("-" + scale + " W", 4, padY - 8 < 0 ? 0 : padY - 8);
  ctx.textBaseline = "bottom";
  ctx.fillText("+" + scale + " W", 4, cssHeight);

  const samples = state.powerHistory;

  if (samples.length === 0)
  {
    powerSparkLayout = null;
    updatePowerTooltip();
    return;
  }

  // One slot per retained sample so the bars keep a steady pitch as the
  // history fills up; the bar stays inside its slot.
  const slot = cssWidth / samples.length;
  const barWidth = Math.max(1, Math.min(10, Math.floor(slot * 0.65)));

  // Mirrored ladder like the used/returned graph panels: the signed-negative
  // consumption watts map above the zero line and the positive feed-in ones
  // below it, so red consumption grows up and green feed-in hangs down.
  function yOf(total)
  {
    const clamped = Math.max(-scale, Math.min(scale, total));
    return mid + (clamped / scale) * halfSpan;
  }

  function centreOf(index)
  {
    return (samples.length === 1) ? (cssWidth / 2) : ((index + 0.5) * slot);
  }

  // Centre-zero bars: consumption grows red above the zero line, feed-in
  // hangs green below it (the same sides the used/returned graph panels
  // use), with a brighter cap at the value end so even the thinnest bars
  // stay readable.
  samples.forEach((sample, index) =>
  {
    const produce = sample.total >= 0;
    const barHeight = Math.max(1, Math.abs(yOf(sample.total) - mid));
    const x = centreOf(index) - barWidth / 2;

    ctx.fillStyle = produce ? "rgba(52, 199, 89, 0.7)" : "rgba(255, 59, 48, 0.7)";
    ctx.fillRect(x, produce ? mid : (mid - barHeight), barWidth, barHeight);

    ctx.fillStyle = produce ? produceColor : consumeColor;
    ctx.fillRect(x, produce ? (mid + barHeight - 1) : (mid - barHeight), barWidth, 1);
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

  // Keep the hover popup in step with the freshly drawn bars (a redraw runs
  // on every power frame and on every resize while the mouse may still sit
  // over the plot).
  powerSparkLayout = {samples: samples, slot: slot, cssWidth: cssWidth};
  updatePowerTooltip();
}

function updatePowerTooltip()
{
  if (powerTip === null)
  {
    powerTip = document.createElement("div");
    powerTip.className = "powerTooltip";
    powerTip.hidden = true;
    el.powerSpark.parentElement.appendChild(powerTip);
  }

  const layout = powerSparkLayout;

  if ((layout === null) || (layout.samples.length === 0) || (powerHoverX === null))
  {
    powerTip.hidden = true;
    return;
  }

  const index = Math.min(layout.samples.length - 1,
                         Math.max(0, Math.floor(powerHoverX / layout.slot)));
  const sample = layout.samples[index];

  // One slot per sample (the drawing's own pitch): the bar centre of the
  // hovered slot; a lone sample sits in the middle like on the canvas.
  const centre = (layout.samples.length === 1) ? (layout.cssWidth / 2) : ((index + 0.5) * layout.slot);

  powerTip.textContent = (sample.time || "--:--:--") + "  ·  " + formatWatt(sample.total);
  powerTip.hidden = false;

  // Centre the popup above the hovered bar and push it back from the edges
  // so it never sticks out of the plot.
  const half = powerTip.offsetWidth / 2;
  const left = Math.min(layout.cssWidth - half, Math.max(half, centre));
  powerTip.style.left = (el.powerSpark.offsetLeft + left) + "px";
  powerTip.style.top = (el.powerSpark.offsetTop + 2) + "px";
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
//-- RingFile view
//-- --------------------------------------------------------------------------

// The device walks the ring its answer names ("ring" echoes the requested
// period) newest to oldest and answers cmd=ringfile&ring=<period> with
// every retained record in that order: the table simply follows the frame.
// Rows are built with textContent only.

// One entry per on-device ring: digits = width of the wall-clock period key
// the device sends (YYMMDDhh / YYMMDD / YYMM), title = first column header,
// unit = singular wording of the state line.
const RINGS =
{
  hours: {digits: 8, title: "Hour", unit: "hour"},
  days: {digits: 6, title: "Day", unit: "day"},
  months: {digits: 4, title: "Month", unit: "month"},
};

function setRingFileState(message)
{
  el.ringFileState.textContent = message;
}

// The ring the radio group currently selects; unknown or missing states
// fall back to the hours default.
function selectedRing()
{
  const checked = document.querySelector('input[name="ringFileChoice"]:checked');

  return (checked && RINGS[checked.value]) ? checked.value : "hours";
}

// Requests the table of the currently selected ring.
function sendRingFile()
{
  wsSend("cmd=ringfile&ring=" + selectedRing());
}

// The wall-clock period key as the user reads it: 26111916 becomes
// "2026-11-19 16" in the hours ring, 261119 becomes "2026-11-19" in the
// days ring and 2611 becomes "2026-11" in the months ring. Text that is
// not all digits is returned unchanged.
function formatRingKey(key, digits)
{
  const text = String(key).padStart(digits, "0");

  if (!/^\d+$/.test(text))
  {
    return text;
  }

  const date = "20" + text.slice(0, 2) + "-" + text.slice(2, 4);

  if (digits <= 4)
  {
    return date;
  }

  const full = date + "-" + text.slice(4, 6);

  return (digits <= 6) ? full : (full + " " + text.slice(6, 8));
}

function renderRingFileList(message)
{
  const tbody = el.ringFileTable.tBodies[0];
  tbody.textContent = "";

  const ring = RINGS[message.ring] ? message.ring : selectedRing();
  const info = RINGS[ring];
  el.ringFileKeyHeader.textContent = info.title;

  const records = message.records || [];

  if (records.length === 0)
  {
    setRingFileState(message.available
                     ? "No " + info.unit + " records stored yet (is the clock synchronised?)."
                     : "The meter store is not running on the device.");
    return;
  }

  setRingFileState(records.length + " " + info.unit +
                   ((records.length === 1) ? " record" : " records") + " shown, newest first" +
                   (message.available ? "" : " (the device walk did not finish)"));

  records.forEach((record) =>
  {
    const row = document.createElement("tr");
    const cells = [
      formatRingKey(record.key, info.digits),
      (typeof record.edT1 === "number") ? record.edT1.toFixed(3) : "-",
      (typeof record.edT2 === "number") ? record.edT2.toFixed(3) : "-",
      (typeof record.erT1 === "number") ? record.erT1.toFixed(3) : "-",
      (typeof record.erT2 === "number") ? record.erT2.toFixed(3) : "-",
      (typeof record.gas === "number") ? record.gas.toFixed(3) : "-",
      (typeof record.ti === "number") ? record.ti.toFixed(2) : "-",
      (typeof record.te === "number") ? record.te.toFixed(2) : "-",
      (typeof record.tG === "number") ? record.tG.toFixed(2) : "-",
      (typeof record.nc === "number") ? record.nc.toFixed(2) : "-",
    ];

    cells.forEach((text, index) =>
    {
      const cell = document.createElement("td");
      cell.textContent = text;

      // The measured columns align right so the digits of successive
      // periods line up; the period column keeps the default left.
      if (index > 0)
      {
        cell.className = "valueNumeric";
      }

      row.appendChild(cell);
    });

    tbody.appendChild(row);
  });
}

//-- --------------------------------------------------------------------------
//-- Period Graph views (Hours, Days and Months)
//-- --------------------------------------------------------------------------

// The device turns the retained cumulative counters of one meter ring into
// per-period amounts (cmd=graph&ring=<hours|days|months>) and sends them
// oldest first, so every graph reads left to right like its mockup: two
// stacked panels on one shared period axis. The upper panel carries one bar
// per period on a zero line: the used kWh (red) grow upwards and the
// returned kWh (green) hang below the zero line, with a centred legend; the
// lower panel carries a blue line of the gas m³ per period with its own
// legend. The months view draws the newest twelve periods only (the
// mockup's December .. November run) and pairs every period with the same
// month of the year before (YY-MM against (YY-1)-MM): the left bar is the
// month one year back, the right bar the period itself. The used pair
// (orange previous, red this period) stands side by side above the zero
// line, the returned pair (light green previous, dark green this period)
// below it, and the gas panel draws both lines (light blue one year back,
// blue this period). A month whose year-ago counterpart the ring no longer
// retains draws its pair alone. Each panel has grid lines, y labels and rotated period
// labels; the label band reserves exactly the room the slanted text
// needs, so nothing is cut off and no dead space is left between the
// panels. Hovering pops up exactly one chip on the energy panel, named
// for and riding along the bar the cursor sits on: above the zero line a
// used (red) bar, below it a returned (green) one. The gas m³ gets one
// chip over the gas panel.
// The state line names the on-device API that serves the underlying ring
// records. Everything is drawn straight from the frame's numbers, nothing
// is interpreted. The toolbar radio group picks the presentation (see
// showPeriodGraphView): Graph draws the canvas above, Table lists the very
// same per-period amounts as a newest-first table (period, used kWh,
// returned kWh and gas m³, plus the row's own Used (€), Returned (€),
// Gas (€) and Total (€) money columns — per row, never a summed row),
// built by renderPeriodGraphTable from the
// cached frame — like the old DSMRlogger32 history tables. The months
// view's Table is the two-year comparison of the old history page
// (assets/oldMonthsTabel.png): the drawn twelve months, newest on top,
// each row pairing every amount with the same month one year back and
// closing with Total and Balance rows. Its third option Costs lists the
// month's money amounts in the same paired layout (assets/
// oldMonthsCosts.png): Electra (the used kWh times the import tariff the
// ring stores), Export (the returned kWh times the export tariff, a credit
// the shared periodCostsOf subtracts in the total), Gas (the gas m³ times
// the gas tariff), Netw.
// (the network amount the ring stores with the record) and the Total, with an
// Advance row of each half's total sum over twelve (the old page's
// Voorschot Bedrag).

const PERIOD_GRAPHS =
{
  hoursGraph: {ring: "hours", unitWord: "hour", api: "/api/v3/ringfile?ring=hours", labelsHeight: 46},
  daysGraph: {ring: "days", unitWord: "day", api: "/api/v3/ringfile?ring=days", labelsHeight: 68},
  monthsGraph: {ring: "months", unitWord: "month", api: "/api/v3/ringfile?ring=months", labelsHeight: 40},
};

const GRAPH_COLORS =
{
  used: "#ff3b30",
  returned: "#34c759",
  usedPrevious: "#ff9500",
  returnedPrevious: "#8ce99a",
  gas: "#007aff",
  gasPrevious: "#a5d8ff",
};

const GRAPH_LAYOUT =
{
  marginLeft: 46,
  marginRight: 10,
  legendHeight: 16,
  panelHeight: 140,
  gasPanelHeight: 100,
  panelGap: 14,
  bottomPad: 8,
};

// Grid ladders: the smallest step whose fifth grid line still reaches the
// panel's maximum (an empty panel keeps the given plain scale).
const KWH_STEPS = [0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000];
const WATT_STEPS = [50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000];

// Per-view rendering state: the newest frame's records and availability
// flag, the presentation mode picked by the view's radio group ("graph"
// draws the canvas, "table" fills the record table, "costs" the months
// view's money table), the drawing geometry of the newest canvas frame
// (for the hover popup), the mouse position inside the canvas (null:
// outside) and the popup chips.
const periodGraphs = {};

Object.keys(PERIOD_GRAPHS).forEach((viewId) =>
{
  periodGraphs[viewId] = {records: [], available: false, mode: "graph",
                          layout: null, hoverX: null, hoverY: null, tips: null};
});

function graphStep(maxValue, steps, emptyStep)
{
  if (!(maxValue > 0))
  {
    return emptyStep;
  }

  for (const step of steps)
  {
    if (maxValue <= step * 5)
    {
      return step;
    }
  }

  return steps[steps.length - 1];
}

function graphScale(maxValue, step)
{
  return Math.max(step, Math.ceil(maxValue / step) * step);
}

function graphViewByRing(ring)
{
  const found = Object.keys(PERIOD_GRAPHS).find((viewId) => PERIOD_GRAPHS[viewId].ring === ring);

  return found || null;
}

function setPeriodGraphState(viewId, message)
{
  el[viewId + "State"].textContent = message;
}

function handleGraphMessage(message)
{
  const viewId = graphViewByRing(message.ring);

  if (viewId !== null)
  {
    renderPeriodGraph(viewId, message);
  }
}

function renderPeriodGraph(viewId, message)
{
  const graph = periodGraphs[viewId];
  graph.records = Array.isArray(message.records) ? message.records : [];
  graph.available = Boolean(message.available);

  showPeriodGraphView(viewId);
}

// Puts the view in the presentation its radio group selects: the record
// table or the canvas with the two stacked panels. The newest frame stays
// in periodGraphs, so switching modes never needs a device round trip.
function showPeriodGraphView(viewId)
{
  const config = PERIOD_GRAPHS[viewId];
  const graph = periodGraphs[viewId];
  const tableMode = (graph.mode === "table");
  const costsMode = (graph.mode === "costs");
  const recordMode = tableMode || costsMode;

  el[viewId + "CanvasWrap"].hidden = recordMode;
  el[viewId + "TableWrap"].hidden = !tableMode;

  if (viewId === "monthsGraph")
  {
    el.monthsGraphCostsTableWrap.hidden = !costsMode;
  }

  if (graph.records.length === 0)
  {
    setPeriodGraphState(viewId, graph.available
                                ? "No complete " + config.unitWord +
                                  " records stored yet (is the clock synchronised?)."
                                : "The meter store is not running on the device.");
    return;
  }

  if (recordMode)
  {
    //-- The months tables show the drawn twelve, the other views every
    // retained period.
    const shown = (viewId === "monthsGraph")
                ? periodGraphWindow(viewId, graph.records).length
                : graph.records.length;

    setPeriodGraphState(viewId, shown + " " + config.unitWord +
                                " records shown, newest first · " + config.api +
                                (graph.available ? "" : " (the device walk did not finish)"));

    if (costsMode)
    {
      renderMonthsCostsTable();
    }
    else
    {
      renderPeriodGraphTable(viewId);
    }
    return;
  }

  //-- The months view plots the newest twelve only, each paired with the
  // same month one year back; the state line names the drawn groups.
  const plotted = periodGraphWindow(viewId, graph.records).length;

  setPeriodGraphState(viewId, plotted + " " + config.unitWord + "(s) plotted" +
                              ((viewId === "monthsGraph")
                               ? ", each paired with the same month one year back"
                               : "") +
                              " · " + config.api +
                              (graph.available ? "" : " (the device walk did not finish)"));

  drawPeriodGraph(viewId);
}

// The period key as the table's first column reads it, styled after the
// old DSMRlogger32 history tables: "19-11 16:00 - 16:59" in the hours
// ring, "Friday 21-11-2026" in the days ring and "November 2026" in the
// months ring. Text that is not the ring's digit pattern is returned
// unchanged.
function periodGraphTableLabel(ring, key)
{
  const digits = (ring === "hours") ? 8 : ((ring === "days") ? 6 : 4);
  const text = String(key).padStart(digits, "0");

  if (!new RegExp("^\\d{" + digits + "}$").test(text))
  {
    return String(key);
  }

  const year2 = Number(text.slice(0, 2));
  const month = Number(text.slice(2, 4));

  if (ring === "months")
  {
    if (month < 1 || month > 12)
    {
      return text;
    }

    return MONTH_NAMES[month - 1] + " 20" + text.slice(0, 2);
  }

  const day = Number(text.slice(4, 6));

  if (ring === "days")
  {
    if (month < 1 || month > 12 || day < 1 || day > daysInMonth2000(year2, month))
    {
      return text;
    }

    return dayNameOf2000(year2, month, day) + " " + text.slice(4, 6) + "-" +
           text.slice(2, 4) + "-20" + text.slice(0, 2);
  }

  return text.slice(4, 6) + "-" + text.slice(2, 4) + " " + text.slice(6, 8) +
         ":00 - " + text.slice(6, 8) + ":59";
}

// The table presentation of the Hours and Days views: one row per
// retained period, newest first (the frame arrives oldest first, so the
// cached array is walked reversed), carrying the same per-period amounts
// the bars are drawn from plus the row's own four money columns: Used
// (€) (the used kWh against the import tariff), Returned (€) (the
// returned kWh against the export tariff, a positive compensation),
// Gas (€) (the gas m³ against the gas tariff) and Total (€) (used minus
// the returned credit plus gas plus the prorated network amount the ring
// stores with the record). Every figure stands
// on its own row; no row sums the table. The months view's table is the
// two-year comparison built by renderMonthsCompareTable. Rows are built
// with textContent only.
function renderPeriodGraphTable(viewId)
{
  if (viewId === "monthsGraph")
  {
    renderMonthsCompareTable();
    return;
  }

  const config = PERIOD_GRAPHS[viewId];
  const tbody = el[viewId + "Table"].tBodies[0];
  tbody.textContent = "";

  periodGraphs[viewId].records.slice().reverse().forEach((record) =>
  {
    //-- The row's money amounts from the shared periodCostsOf — the very
    // same formula the months Costs table prints (the used kWh against
    // the import tariff, the returned kWh against the export tariff as a
    // positive credit the total nets, the gas m³ against the gas tariff
    // and the period's prorated network share; total used minus returned
    // plus gas plus network). A dash prints only when the record carries
    // none of the four figures.
    const costs = periodCostsOf(record);

    const cells = [
      periodGraphTableLabel(config.ring, record.key),
      (typeof record.used === "number") ? record.used.toFixed(3) : "-",
      (typeof record.returned === "number") ? record.returned.toFixed(3) : "-",
      (typeof record.gas === "number") ? record.gas.toFixed(3) : "-",
      graphCellNumber(costs.electra, 2),
      graphCellNumber(costs.exported, 2),
      graphCellNumber(costs.gas, 2),
      graphCellNumber(costs.total, 2),
    ];

    appendGraphTableRow(tbody, cells);
  });
}

const DAY_NAMES = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
const MONTH_NAMES = ["January", "February", "March", "April", "May", "June", "July", "August",
                     "September", "October", "November", "December"];

// Days of a 2-digit year (20YY): the calendar the period keys need to ask
// whether two keys name directly consecutive periods.
function daysInMonth2000(year2, month)
{
  const daysPerMonth = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];

  if ((month === 2) && (((2000 + year2) % 4) === 0) &&
      (((2000 + year2) % 100) !== 0 || ((2000 + year2) % 400) === 0))
  {
    return 29;
  }

  return (month >= 1 && month <= 12) ? daysPerMonth[month - 1] : 31;
}

// Weekday of a 20YY calendar day (Zeller's congruence), as DAY_NAMES index.
function dayNameOf2000(year2, month, day)
{
  let year = 2000 + year2;
  let monthIndex = month;

  if (monthIndex < 3)
  {
    monthIndex += 12;
    year -= 1;
  }

  const century = Math.floor(year / 100);
  const yearOfCentury = year % 100;
  const zeller = (day + Math.floor((13 * (monthIndex + 1)) / 5) + yearOfCentury +
                  Math.floor(yearOfCentury / 4) + Math.floor(century / 4) + (5 * century)) % 7;

  return DAY_NAMES[(zeller + 6) % 7];
}

// The period key as the axis reads it: 26111916 becomes "19-11 16:00" on
// the hours ring, 261119 becomes "Thursday 22-10" on the days ring and 2611
// becomes "November" on the months ring (January carries the two-digit
// year, so every year opens under a named month).
function periodGraphLabel(ring, key)
{
  const digits = (ring === "hours") ? 8 : ((ring === "days") ? 6 : 4);
  const text = String(key).padStart(digits, "0");

  if (!new RegExp("^\\d{" + digits + "}$").test(text))
  {
    return String(key);
  }

  const year2 = Number(text.slice(0, 2));
  const month = Number(text.slice(2, 4));

  if (ring === "months")
  {
    if (month < 1 || month > 12)
    {
      return text;
    }

    return (month === 1) ? (MONTH_NAMES[0] + " " + text.slice(0, 2)) : MONTH_NAMES[month - 1];
  }

  const day = Number(text.slice(4, 6));

  if (ring === "days")
  {
    const name = dayNameOf2000(year2, month, day);

    if (day < 1 || day > daysInMonth2000(year2, month) || month < 1 || month > 12)
    {
      return text;
    }

    return name + " " + text.slice(4, 6) + "-" + text.slice(2, 4);
  }

  return text.slice(4, 6) + "-" + text.slice(2, 4) + " " + text.slice(6, 8) + ":00";
}

// One panel's furniture: the centred legend row above it, the horizontal
// grid lines with their y labels, the zero baseline, the left axis and the
// unit above the axis. The scale spans scaleBottom .. scaleTop (both on the
// shared step; scaleBottom is negative when the panel hangs bars below the
// zero line, zero for a plain 0-based panel). The bars or line of the panel
// are drawn by the caller on top of this.
function drawGraphPanelFrame(ctx, panel, step, scaleTop, scaleBottom, unit, legendEntries, plotLeft, plotWidth)
{
  const gridColor = cssVar("--separator", "rgba(60, 60, 67, 0.12)");
  const axisColor = cssVar("--hairline", "rgba(60, 60, 67, 0.29)");
  const secondaryColor = cssVar("--secondary", "#6e6e73");
  const decimals = (step >= 1) ? 0 : 1;
  const range = (scaleTop - scaleBottom) || step;

  function yOf(value)
  {
    return panel.y + ((scaleTop - value) / range) * panel.height;
  }

  // Centred legend row: colour chip + name per entry.
  ctx.font = "10px " + cssVar("--font", "sans-serif");
  let rowWidth = 0;
  legendEntries.forEach((entry) =>
  {
    rowWidth += 11 + 4 + ctx.measureText(entry.label).width + 16;
  });
  let legendX = plotLeft + Math.max(0, (plotWidth - rowWidth) / 2);

  legendEntries.forEach((entry) =>
  {
    ctx.fillStyle = entry.color;
    ctx.fillRect(legendX, panel.y - 13, 11, 11);
    ctx.fillStyle = secondaryColor;
    ctx.textAlign = "left";
    ctx.textBaseline = "middle";
    ctx.fillText(entry.label, legendX + 15, panel.y - 7);
    legendX += 11 + 4 + ctx.measureText(entry.label).width + 16;
  });

  // Grid lines every step above and below the zero line, labels left of
  // the axis; the amounts below the line are the returned kWh, so they are
  // labelled as the negative mirror of their magnitude.
  ctx.font = "9px " + cssVar("--mono", "monospace");
  ctx.lineWidth = 0.5;
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";

  for (let value = step; value <= scaleTop + (step / 100); value += step)
  {
    const y = yOf(value);
    ctx.strokeStyle = gridColor;
    ctx.beginPath();
    ctx.moveTo(plotLeft, y);
    ctx.lineTo(plotLeft + plotWidth, y);
    ctx.stroke();
    ctx.fillStyle = secondaryColor;
    ctx.fillText(value.toFixed(decimals), plotLeft - 4, y);
  }

  for (let value = step; value <= -scaleBottom + (step / 100); value += step)
  {
    const y = yOf(-value);
    ctx.strokeStyle = gridColor;
    ctx.beginPath();
    ctx.moveTo(plotLeft, y);
    ctx.lineTo(plotLeft + plotWidth, y);
    ctx.stroke();
    ctx.fillStyle = secondaryColor;
    ctx.fillText((-value).toFixed(decimals), plotLeft - 4, y);
  }

  //-- Zero line: the baseline the used bars grow from and the returned
  // bars hang from (the panel bottom itself when the scale ends at zero).
  ctx.strokeStyle = axisColor;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(plotLeft, yOf(0));
  ctx.lineTo(plotLeft + plotWidth, yOf(0));
  ctx.moveTo(plotLeft, panel.y + panel.height);
  ctx.lineTo(plotLeft + plotWidth, panel.y + panel.height);
  ctx.moveTo(plotLeft, panel.y);
  ctx.lineTo(plotLeft, panel.y + panel.height);
  ctx.stroke();

  ctx.fillStyle = secondaryColor;
  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  ctx.fillText(unit, 6, panel.y - 2);

  return yOf;
}

// Rotated period labels under a panel, hung from their tick to the lower
// left like the mockups. Every k-th slot gets one: k is picked from the
// widest label (its slant runs 0.707 of its width down and as far to the
// left), so the slanted text never collides at narrow pitches and never
// needs more than the panel's labelsHeight reserve. firstSlot shifts the
// ticks right, so a right-aligned series (the Actual Graph's newest sample
// in the last slot) gets its labels under its bars and not under the empty
// left of the plot.
function drawGraphTimeLabels(ctx, records, belowY, slot, plotLeft, labelOf, firstSlot)
{
  ctx.save();
  ctx.font = "9px " + cssVar("--mono", "monospace");
  ctx.fillStyle = cssVar("--secondary", "#6e6e73");
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";

  let widest = 0;

  records.forEach((record) =>
  {
    widest = Math.max(widest, ctx.measureText(String(labelOf(record))).width);
  });

  const labelEvery = Math.max(1, Math.ceil((widest * Math.SQRT1_2 + 8) / slot));

  records.forEach((record, index) =>
  {
    if ((index % labelEvery) !== 0)
    {
      return;
    }

    ctx.save();
    ctx.translate(plotLeft + ((firstSlot || 0) + index + 0.5) * slot, belowY + 6);
    ctx.rotate(-Math.PI / 4);
    ctx.fillText(String(labelOf(record)), 0, 0);
    ctx.restore();
  });

  ctx.restore();
}

// Blue line of a panel value with a dot per record (the gas series of the
// period graphs and the Actual Graph). Null values start a new segment, so
// the previous-period line of the months graph simply breaks where the
// ring holds no predecessor.
function drawGraphLine(ctx, values, firstSlot, slot, plotLeft, yOf, color)
{
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.5;
  ctx.beginPath();

  let open = false;

  values.forEach((value, index) =>
  {
    if (value === null)
    {
      open = false;
      return;
    }

    const x = plotLeft + (firstSlot + index + 0.5) * slot;
    const y = yOf(value);

    if (open)
    {
      ctx.lineTo(x, y);
    }
    else
    {
      ctx.moveTo(x, y);
      open = true;
    }
  });

  ctx.stroke();

  ctx.fillStyle = color;

  values.forEach((value, index) =>
  {
    if (value === null)
    {
      return;
    }

    ctx.beginPath();
    ctx.arc(plotLeft + (firstSlot + index + 0.5) * slot, yOf(value), 1.8, 0, Math.PI * 2);
    ctx.fill();
  });
}

// The record that pairs with a drawn period on the months view: the ring
// entry of the same month one year back (the YYMM key minus a hundred, so
// 2611 pairs with 2511), or null when the ring no longer retains that
// month. The lookup runs over all retained records, not just the drawn
// window: the year-ago months live outside the newest twelve.
function periodYearAgoRecordByKey(records, key)
{
  const yearAgoKey = Number(key) - 100;

  if (yearAgoKey < 0)
  {
    return null;
  }

  return records.find((candidate) => Number(candidate.key) === yearAgoKey) || null;
}

// Months view window: the graph draws the newest twelve periods, the run
// the mockup shows (December .. November). Every retained month older than
// that only serves as the pair partner of its own successor. Fewer
// retained periods simply draw fewer groups; the other views are untouched.
const MONTHS_WINDOW_SIZE = 12;

function periodGraphWindow(viewId, records)
{
  if ((viewId !== "monthsGraph") || (records.length <= MONTHS_WINDOW_SIZE))
  {
    return records;
  }

  return records.slice(records.length - MONTHS_WINDOW_SIZE);
}

// One table row for the period views: every cell carries plain text and
// the value columns (everything but the leading label) align right so the
// digits of successive periods line up. A footer row (the Total, Balance
// and Advance rows of the months tables) gets the .tableFooterRow style.
function appendGraphTableRow(tbody, cells, footer)
{
  const row = document.createElement("tr");

  if (footer)
  {
    row.className = "tableFooterRow";
  }

  cells.forEach((text, index) =>
  {
    const cell = document.createElement("td");
    cell.textContent = text;

    if (index > 0)
    {
      cell.className = "valueNumeric";
    }

    row.appendChild(cell);
  });

  tbody.appendChild(row);
}

// Cell text of a measured amount: fixed decimals, or a dash when the
// ring holds no value for it (a year the pairing has nothing for).
function graphCellNumber(value, digits)
{
  return ((typeof value === "number") && Number.isFinite(value)) ? value.toFixed(digits) : "-";
}

// The months tables' period key split: the month name and the "20YY"
// year labels of the month and of the same month one year back (year
// wrap included) from a YYMM key; null when the text is not four digits.
function monthsTableParts(key)
{
  const text = String(key).padStart(4, "0");

  if (!new RegExp("^\\d{4}$").test(text))
  {
    return null;
  }

  const year2 = Number(text.slice(0, 2));
  const month = Number(text.slice(2, 4));

  if (month < 1 || month > 12)
  {
    return null;
  }

  return {monthName: MONTH_NAMES[month - 1],
          yearLabel: "20" + text.slice(0, 2),
          yearAgoLabel: "20" + String((year2 + 99) % 100).padStart(2, "0")};
}

// The months view's Table presentation (the old DSMRlogger32 history
// page, assets/oldMonthsTabel.png): the drawn twelve months newest on
// top, every row showing used, returned and gas twice — this month and
// the same month one year back (the partner is looked up over all
// retained records, so the year-ago months may live outside the window);
// a year the ring no longer retains shows dashes. The Total row sums the
// drawn rows column by column, the Balance row nets used against
// returned for each year. Rows are built with textContent only.
function renderMonthsCompareTable()
{
  const tbody = el.monthsGraphTable.tBodies[0];
  tbody.textContent = "";

  const records = periodGraphs.monthsGraph.records;
  const drawn = periodGraphWindow("monthsGraph", records).slice().reverse();

  const totals = {used: 0, returned: 0, gas: 0, yearAgoUsed: 0, yearAgoReturned: 0, yearAgoGas: 0};

  const addTo = (value) => ((typeof value === "number") && Number.isFinite(value)) ? value : 0;

  drawn.forEach((record) =>
  {
    const parts = monthsTableParts(record.key);

    if (parts === null)
    {
      appendGraphTableRow(tbody, [String(record.key)]);
      return;
    }

    const previous = periodYearAgoRecordByKey(records, record.key);

    totals.used += addTo(record.used);
    totals.returned += addTo(record.returned);
    totals.gas += addTo(record.gas);
    totals.yearAgoUsed += addTo(previous ? previous.used : null);
    totals.yearAgoReturned += addTo(previous ? previous.returned : null);
    totals.yearAgoGas += addTo(previous ? previous.gas : null);

    appendGraphTableRow(tbody,
    [
      parts.monthName,
      parts.yearLabel, graphCellNumber(record.used, 3),
      parts.yearAgoLabel, graphCellNumber(previous ? previous.used : null, 3),
      parts.yearLabel, graphCellNumber(record.returned, 3),
      parts.yearAgoLabel, graphCellNumber(previous ? previous.returned : null, 3),
      parts.yearLabel, graphCellNumber(record.gas, 3),
      parts.yearAgoLabel, graphCellNumber(previous ? previous.gas : null, 3),
    ]);
  });

  appendGraphTableRow(tbody,
  [
    "Total",
    "", graphCellNumber(totals.used, 3), "", graphCellNumber(totals.yearAgoUsed, 3),
    "", graphCellNumber(totals.returned, 3), "", graphCellNumber(totals.yearAgoReturned, 3),
    "", graphCellNumber(totals.gas, 3), "", graphCellNumber(totals.yearAgoGas, 3),
  ], true);

  appendGraphTableRow(tbody,
  [
    "Balance",
    "", graphCellNumber(totals.used - totals.returned, 3),
    "", graphCellNumber(totals.yearAgoUsed - totals.yearAgoReturned, 3),
    "", "", "", "", "", "", "", "",
  ], true);
}

// One Costs money column: the period's quantity times the tariff the ring
// stores for that direction (kWh against the EUR/kWh tariff, gas m³ against
// the EUR/m³ tariff) — the amount the month's energy costs or earns. Null
// when either figure is missing, so the row prints dashes instead of
// inventing a figure.
function monthsProductEuro(quantity, tariff)
{
  const isFigure = (value) => ((typeof value === "number") && Number.isFinite(value));

  return (isFigure(quantity) && isFigure(tariff)) ? quantity * tariff : null;
}

// The month's Electra amount in EUR: the used kWh times the import tariff
// the ring stores under the ti rubric (what the grid delivered to the home
// costs), or null when the record carries neither figure.
function monthsElectraEuro(record)
{
  return monthsProductEuro(record.used, record.ti);
}

// The month's export credit in EUR: the returned kWh times the export
// tariff the ring stores under the te rubric (what the delivered-back
// energy earns — the net compensation per delivered-back kWh, a positive
// figure), or null when the record carries neither figure. The Costs
// presentations print it as its own column and periodCostsOf nets it as
// a credit in the total.
function monthsExportEuro(record)
{
  return monthsProductEuro(record.returned, record.te);
}

// The month's network and transport amount in EUR (the Netw. column): the
// figure the ring stores with the record (the Settings "Network & transport
// costs (month)" amount as it was when the record was written), or null
// when the record carries none.
function monthsNetworkCostsEuro(record)
{
  return ((typeof record.nc === "number") && Number.isFinite(record.nc)) ? record.nc : null;
}

// The single money formula every period view shares (the months Costs
// table and the Hours and Days tables' four amount columns alike): the
// used kWh against the import tariff (electra), the returned kWh against
// the export tariff (exported — the net compensation per delivered-back
// kWh, a positive figure), the gas m³ against the gas tariff (gas) and
// the period's prorated network share the ring stores with the record
// (netw). The total nets the export credit from the costs, exactly the
// rule dynamicPricing.md documents for the Costs presentations: used
// minus returned plus gas plus network. All five fields are null when
// the record carries none of the figures.
function periodCostsOf(record)
{
  const electra = monthsElectraEuro(record);
  const exported = monthsExportEuro(record);
  const gas = monthsProductEuro(record.gas, record.tG);
  const netw = monthsNetworkCostsEuro(record);

  if ((electra === null) && (exported === null) && (gas === null) && (netw === null))
  {
    return {electra: null, exported: null, gas: null, netw: null, total: null};
  }

  return {
    electra: electra,
    exported: exported,
    gas: gas,
    netw: netw,
    total: (electra || 0) - (exported || 0) + (gas || 0) + (netw || 0)
  };
}

// The months view's Costs presentation (the old DSMRlogger32 history
// page, assets/oldMonthsCosts.png): per drawn month the money amounts,
// each the period's quantity times the tariff the ring stores — Electra
// (the used kWh times the import tariff, what the used energy costs),
// Export (the returned kWh times the export tariff, the credit the
// delivered-back energy earns), Gas (the gas m³ times the gas tariff),
// Netw. (the network and transport amount the ring stores with the record,
// the Settings month amount as it was when the record was written) and
// their Total (Electra minus Export plus Gas plus Netw. from the shared
// periodCostsOf — the export amount is a positive compensation the total
// nets as a credit) —
// each month paired with the same month one year back, the ring partner
// supplying the right half. A month the ring does not retain
// shows dashes. The Advance row prints each half's advance payment: that
// column's total sum divided by twelve (the old page's Voorschot
// Bedrag). Rows are built with textContent only.
function renderMonthsCostsTable()
{
  const tbody = el.monthsGraphCostsTable.tBodies[0];
  tbody.textContent = "";

  const records = periodGraphs.monthsGraph.records;
  const drawn = periodGraphWindow("monthsGraph", records).slice().reverse();

  let columnTotal = 0;
  let columnTotalYearAgo = 0;

  drawn.forEach((record) =>
  {
    const parts = monthsTableParts(record.key);

    if (parts === null)
    {
      appendGraphTableRow(tbody, [String(record.key)]);
      return;
    }

    const previous = periodYearAgoRecordByKey(records, record.key);
    const costs = periodCostsOf(record);
    const yearAgo = previous
        ? periodCostsOf(previous)
        : {electra: null, exported: null, gas: null, netw: null, total: null};

    columnTotal += costs.total || 0;
    columnTotalYearAgo += yearAgo.total || 0;

    appendGraphTableRow(tbody,
    [
      parts.monthName,
      parts.yearLabel,
      graphCellNumber(costs.electra, 2), graphCellNumber(costs.exported, 2),
      graphCellNumber(costs.gas, 2),
      graphCellNumber(costs.netw, 2), graphCellNumber(costs.total, 2),
      parts.yearAgoLabel,
      graphCellNumber(yearAgo.electra, 2), graphCellNumber(yearAgo.exported, 2),
      graphCellNumber(yearAgo.gas, 2),
      previous ? graphCellNumber(yearAgo.netw, 2) : "-", graphCellNumber(yearAgo.total, 2),
    ]);
  });

  appendGraphTableRow(tbody,
  [
    "", "", "", "", "", "Advance amount", graphCellNumber(columnTotal / 12, 2),
    "", "", "", "", "Advance amount", graphCellNumber(columnTotalYearAgo / 12, 2),
  ], true);
}

// Used/returned bars on the energy panel: one bar pair per record (red
// upwards from the zero line, green hanging below it).
function drawPeriodBars(ctx, records, slot, plotLeft, yOfEnergy)
{
  const barWidth = Math.max(2, slot * 0.6);
  const zeroY = yOfEnergy(0);

  records.forEach((record, index) =>
  {
    const centre = plotLeft + (index + 0.5) * slot;
    const barX = centre - barWidth / 2;

    const entries =
    [
      {value: record.used || 0, color: GRAPH_COLORS.used, to: yOfEnergy(record.used || 0)},
      {value: record.returned || 0, color: GRAPH_COLORS.returned, to: yOfEnergy(-(record.returned || 0))},
    ];

    entries.forEach((bar) =>
    {
      if (bar.value <= 0)
      {
        return;
      }

      const top = Math.min(zeroY, bar.to);
      const bottom = Math.max(zeroY, bar.to);
      ctx.fillStyle = bar.color;
      ctx.globalAlpha = 0.85;
      ctx.fillRect(barX, top, barWidth, bottom - top);
      ctx.globalAlpha = 1;
    });
  });
}

// Months view: every drawn period stands as one group of two bars: the
// left bar the same month of the year before (orange used, light green
// returned), the right bar the period itself (red used, dark green
// returned). The year-ago partner is looked up among all retained records,
// not just the drawn window. A period whose year-ago month the ring no
// longer retains draws its pair alone, like the mockup's first months.
function drawPeriodBarsPaired(ctx, drawn, records, slot, plotLeft, yOfEnergy)
{
  const pairWidth = Math.max(4, slot * 0.7);
  const barWidth = Math.max(1, (pairWidth - 1) / 2);
  const zeroY = yOfEnergy(0);

  drawn.forEach((record, index) =>
  {
    const centre = plotLeft + (index + 0.5) * slot;
    const leftX = centre - pairWidth / 2;
    const rightX = leftX + barWidth + 1;
    const previous = periodYearAgoRecordByKey(records, record.key);

    const bars =
    [
      {x: leftX, value: previous ? (previous.used || 0) : 0, color: GRAPH_COLORS.usedPrevious, above: true},
      {x: rightX, value: record.used || 0, color: GRAPH_COLORS.used, above: true},
      {x: leftX, value: previous ? (previous.returned || 0) : 0, color: GRAPH_COLORS.returnedPrevious, above: false},
      {x: rightX, value: record.returned || 0, color: GRAPH_COLORS.returned, above: false},
    ];

    bars.forEach((bar) =>
    {
      if (bar.value <= 0)
      {
        return;
      }

      const end = bar.above ? yOfEnergy(bar.value) : yOfEnergy(-bar.value);
      const top = Math.min(zeroY, end);
      const bottom = Math.max(zeroY, end);
      ctx.fillStyle = bar.color;
      ctx.globalAlpha = 0.85;
      ctx.fillRect(bar.x, top, barWidth, bottom - top);
      ctx.globalAlpha = 1;
    });
  });
}

function drawPeriodGraph(viewId)
{
  const canvas = el[viewId + "Canvas"];

  if (canvas === null)
  {
    return;
  }

  const config = PERIOD_GRAPHS[viewId];
  const graph = periodGraphs[viewId];
  const wrap = canvas.parentElement;
  const cssWidth = Math.max(320, wrap.clientWidth - 16);
  const cssHeight = GRAPH_LAYOUT.legendHeight + GRAPH_LAYOUT.panelHeight + config.labelsHeight +
                    GRAPH_LAYOUT.panelGap + GRAPH_LAYOUT.legendHeight +
                    GRAPH_LAYOUT.gasPanelHeight + config.labelsHeight + GRAPH_LAYOUT.bottomPad;
  const dpr = window.devicePixelRatio || 1;

  canvas.width = Math.round(cssWidth * dpr);
  canvas.height = Math.round(cssHeight * dpr);
  canvas.style.width = cssWidth + "px";
  canvas.style.height = cssHeight + "px";

  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cssWidth, cssHeight);

  const records = graph.records;

  //-- Months view: only the newest twelve periods take the axis (one paired
  // group each); every older retained month only serves as a pair partner.
  const paired = (viewId === "monthsGraph");
  const drawn = paired ? periodGraphWindow(viewId, records) : records;

  const plotLeft = GRAPH_LAYOUT.marginLeft;
  const plotWidth = Math.max(10, cssWidth - plotLeft - GRAPH_LAYOUT.marginRight);
  const slot = (drawn.length > 0) ? (plotWidth / drawn.length) : plotWidth;

  // Both panels share the period axis; the lower one sits under the rotated
  // labels of the upper panel with its own legend row in between. The label
  // band is exactly as tall as the slanted text needs (see
  // drawGraphTimeLabels), so no dead space survives between the panels.
  const panel1 =
  {
    y: GRAPH_LAYOUT.legendHeight,
    height: GRAPH_LAYOUT.panelHeight,
  };
  const panel2 =
  {
    y: panel1.y + panel1.height + config.labelsHeight + GRAPH_LAYOUT.panelGap +
       GRAPH_LAYOUT.legendHeight,
    height: GRAPH_LAYOUT.gasPanelHeight,
  };

  let maxUsed = 0;
  let maxReturned = 0;
  let maxGas = 0;

  drawn.forEach((record) =>
  {
    maxUsed = Math.max(maxUsed, record.used || 0);
    maxReturned = Math.max(maxReturned, record.returned || 0);
    maxGas = Math.max(maxGas, record.gas || 0);

    //-- The months view adds the left bars and the light blue line of the
    // same month one year back (outside the drawn window): their amounts
    // have to be on the scales too or they would tower over the panels.
    if (paired)
    {
      const previous = periodYearAgoRecordByKey(records, record.key);

      if (previous !== null)
      {
        maxUsed = Math.max(maxUsed, previous.used || 0);
        maxReturned = Math.max(maxReturned, previous.returned || 0);
        maxGas = Math.max(maxGas, previous.gas || 0);
      }
    }
  });

  //-- One kWh scale serves both directions so a red and a green bar of the
  // same value have the same length; the zero line ends up between the
  // scaled used span above and the scaled returned span below.
  const kwhStep = graphStep(Math.max(maxUsed, maxReturned), KWH_STEPS, 1);
  const kwhTop = (maxUsed > 0) ? graphScale(maxUsed, kwhStep) : 0;
  const kwhBottom = (maxReturned > 0) ? -graphScale(maxReturned, kwhStep) : 0;

  // An empty panel still needs a scale to sit its zero line on.
  if ((kwhTop === 0) && (kwhBottom === 0))
  {
    kwhTop = kwhStep;
  }
  const gasStep = graphStep(maxGas, KWH_STEPS, 1);
  const gasMax = graphScale(maxGas, gasStep);

  //-- Upper panel: the used/returned bars (grouped per period on the months
  // view) on the zero line.
  const yOfEnergy = drawGraphPanelFrame(ctx, panel1, kwhStep, kwhTop, kwhBottom, "kWh",
                                        paired
                                        ? [{color: GRAPH_COLORS.used, label: "Used (this period)"},
                                           {color: GRAPH_COLORS.usedPrevious, label: "Used (previous)"},
                                           {color: GRAPH_COLORS.returned, label: "Returned (this period)"},
                                           {color: GRAPH_COLORS.returnedPrevious, label: "Returned (previous)"}]
                                        : [{color: GRAPH_COLORS.used, label: "Used"},
                                           {color: GRAPH_COLORS.returned, label: "Returned"}],
                                        plotLeft, plotWidth);

  if (paired)
  {
    drawPeriodBarsPaired(ctx, drawn, records, slot, plotLeft, yOfEnergy);
  }
  else
  {
    drawPeriodBars(ctx, drawn, slot, plotLeft, yOfEnergy);
  }

  drawGraphTimeLabels(ctx, drawn, panel1.y + panel1.height, slot, plotLeft,
                      (record) => periodGraphLabel(config.ring, record.key));

  //-- Lower panel: the gas m³ per period as a blue line (the months view
  // adds the light blue line of the same months one year back).
  const yOfGas = drawGraphPanelFrame(ctx, panel2, gasStep, gasMax, 0, "m³",
                                     paired
                                     ? [{color: GRAPH_COLORS.gas, label: "Gas (this period)"},
                                        {color: GRAPH_COLORS.gasPrevious, label: "Gas (previous)"}]
                                     : [{color: GRAPH_COLORS.gas, label: "Gas used"}],
                                     plotLeft, plotWidth);

  if (drawn.length > 0)
  {
    drawGraphLine(ctx, drawn.map((record) => record.gas || 0), 0, slot, plotLeft, yOfGas,
                  GRAPH_COLORS.gas);

    if (paired)
    {
      drawGraphLine(ctx, drawn.map((record) =>
      {
        const previous = periodYearAgoRecordByKey(records, record.key);

        return previous ? (previous.gas || 0) : null;
      }), 0, slot, plotLeft, yOfGas, GRAPH_COLORS.gasPrevious);
    }
  }

  drawGraphTimeLabels(ctx, drawn, panel2.y + panel2.height, slot, plotLeft,
                      (record) => periodGraphLabel(config.ring, record.key));

  graph.layout =
  {
    records: drawn,
    //-- All retained records behind the drawn window: the months view looks
    // up each group's year-ago partner here.
    allRecords: records,
    slot: slot,
    plotLeft: plotLeft,
    cssWidth: cssWidth,
    panelEnergy: panel1,
    panelGas: panel2,
    // Cursor above this y belongs to the kWh panel, below it to the gas
    // panel (the rotated period labels in between stay with the kWh panel).
    panelSplitY: (panel1.y + panel1.height + panel2.y) / 2,
    yOfEnergy: yOfEnergy,
    yOfGas: yOfGas,
  };
  updatePeriodGraphTooltip(viewId);
}

// Hover popups: separate chips per series (same chip style as the power
// history popup). Every view reads one bar at a time: while the cursor is
// over the energy panel it gets exactly one chip, for the bar it sits on
// (above the zero line that bar is a used (red) bar, below it a returned
// (green) one), riding along that bar. The months view additionally
// splits the slot in two columns (left the same month one year back,
// right this period); the hours and days views read the hovered period's
// own bars. Over the gas panel a single chip shows the gas m³. The chips
// centre on their own bar and stay inside the plot.
function updatePeriodGraphTooltip(viewId)
{
  const graph = periodGraphs[viewId];
  const canvas = el[viewId + "Canvas"];

  if (canvas === null)
  {
    return;
  }

  const paired = (viewId === "monthsGraph");

  if (graph.tips === null)
  {
    graph.tips = {};

    // The months view rides four energy bars (this period and the same
    // month of last year, used and returned), the other views two.
    const series = [["used", "tipUsed"], ["returned", "tipReturned"]];

    if (paired)
    {
      series.push(["usedPrevious", "tipUsedPrevious"], ["returnedPrevious", "tipReturnedPrevious"]);
    }

    series.push(["gas", "tipGas"]);

    series.forEach((entry) =>
    {
      const tip = document.createElement("div");
      tip.className = "hoursGraphTooltip " + entry[1];
      tip.hidden = true;
      canvas.parentElement.appendChild(tip);
      graph.tips[entry[0]] = tip;
    });
  }

  const tips = graph.tips;

  Object.keys(tips).forEach((name) =>
  {
    tips[name].hidden = true;
  });

  const layout = graph.layout;

  if ((layout === null) || (layout.records.length === 0) ||
      (graph.hoverX === null) || (graph.hoverY === null))
  {
    return;
  }

  const index = Math.min(layout.records.length - 1,
                         Math.max(0, Math.floor((graph.hoverX - layout.plotLeft) / layout.slot)));
  const record = layout.records[index];
  const centre = layout.plotLeft + (index + 0.5) * layout.slot;

  function place(tip, top, x)
  {
    tip.hidden = false;

    const half = tip.offsetWidth / 2;
    const left = Math.min(layout.cssWidth - half, Math.max(half, x));
    tip.style.left = (canvas.offsetLeft + left) + "px";
    tip.style.top = (canvas.offsetTop + top) + "px";
  }

  if (graph.hoverY >= layout.panelSplitY)
  {
    tips.gas.textContent = (record.gas || 0).toFixed(3) + " m³";
    place(tips.gas, layout.panelGas.y + 2, centre);
    return;
  }

  const energy = layout.panelEnergy;

  if (!paired)
  {
    // Hours and days views read one bar at a time, like the months view:
    // above the zero line the cursor sits on the used (red) bar, below it
    // on the returned (green) one, and only that bar's chip shows.
    const isUsed = (graph.hoverY < layout.yOfEnergy(0));
    const tip = isUsed ? tips.used : tips.returned;
    const value = (isUsed ? record.used : record.returned) || 0;

    tip.textContent = (isUsed ? "Used " : "Returned ") + value.toFixed(3) + " kWh";

    // The chip rides along its own bar (clamped inside the panel), so the
    // used chip reads with the red series and the returned chip with the
    // green one.
    const chipTop = isUsed
                  ? Math.max(energy.y + 2,
                             Math.min(layout.yOfEnergy(value) - 22, energy.y + energy.height - 22))
                  : Math.max(energy.y + 2,
                             Math.min(layout.yOfEnergy(-value) + 3, energy.y + energy.height - 22));

    place(tip, chipTop, centre);

    return;
  }

  // Months view: one chip for the hovered bar only. The slot geometry
  // mirrors drawPeriodBarsPaired (left column the same month of last year,
  // right column this period); above the zero line the cursor reads a
  // used bar, below it a returned one.
  const pairWidth = Math.max(4, layout.slot * 0.7);
  const barWidth = Math.max(1, (pairWidth - 1) / 2);
  const leftX = centre - pairWidth / 2;
  const rightX = leftX + barWidth + 1;

  const fromPrevious = (graph.hoverX < centre);
  const previous = periodYearAgoRecordByKey(layout.allRecords, record.key);

  // The left column only holds bars when the ring retains the month of
  // last year; hovering that empty column shows nothing.
  if (fromPrevious && (previous === null))
  {
    return;
  }

  const source = fromPrevious ? previous : record;
  const isUsed = (graph.hoverY < layout.yOfEnergy(0));
  const value = (isUsed ? source.used : source.returned) || 0;
  const tipName = isUsed ? (fromPrevious ? "usedPrevious" : "used")
                         : (fromPrevious ? "returnedPrevious" : "returned");
  const label = (isUsed ? "Used" : "Returned") +
                (fromPrevious ? " (previous)" : " (this period)");

  tips[tipName].textContent = label + " " + value.toFixed(3) + " kWh";

  const chipTop = isUsed
                ? Math.max(energy.y + 2,
                           Math.min(layout.yOfEnergy(value) - 22, energy.y + energy.height - 22))
                : Math.max(energy.y + 2,
                           Math.min(layout.yOfEnergy(-value) + 3, energy.y + energy.height - 22));

  place(tips[tipName], chipTop, (fromPrevious ? rightX : leftX) + barWidth / 2);
}

//-- --------------------------------------------------------------------------
//-- Actual Graph view
//-- --------------------------------------------------------------------------

// The device keeps an internal rolling table of the newest accepted
// telegrams (the receiver power history): per sample the unsigned delivered
// and returned watts of each phase, the held gas m³/h rate and the
// "HH:MM:SS" time. The table holds a hundred samples, twice that whenever
// the heap carried the wider table at boot; cmd=actualgraph echoes the
// real slot count as "capacity" and the view plots on that pitch. The view
// draws the samples right aligned in the full-capacity plot, so the newest
// measurements fill the graph at the right edge and a full table shifts one
// slot to the left with every accepted telegram. Three used
// (L1..L3) and three returned (L1..L3) checkboxes switch each phase's
// stacked series on and off; the watt scale follows the visible series
// only. The lower panel carries the gas line, like the actualGraph mockup.
// The view follows the telegram cadence: the device pushes nothing here, so
// the SPA polls cmd=actualgraph on the Phase Load timer while it is open.

// Baseline plot width: the device reports its real table size in every
// cmd=actualgraph frame (message.capacity) and the view adopts it, so this
// value only serves until the first frame arrives.
const ACTUAL_GRAPH =
{
  capacity: 100,
  marginLeft: 46,
  marginRight: 10,
  legendHeight: 16,
  panelHeight: 150,
  gasPanelHeight: 90,
  labelsHeight: 40,
  panelGap: 14,
  bottomPad: 8,
};

// Three shades per direction like the actualGraph mockup: light L1, mid
// L2, dark L3, so the stacked series stay readable on top of each other.
const ACTUAL_COLORS =
{
  used: ["#ffc2c2", "#ff453a", "#b4000e"],
  returned: ["#a7f3c0", "#34c759", "#0a7d2c"],
  gas: "#007aff",
};

const actualGraph = {records: [], layout: null, hoverX: null, hoverY: null, tip: null};
let actualGraphTimer = null;

function setActualGraphState(message)
{
  el.actualGraphState.textContent = message;
}

function renderActualGraph(message)
{
  actualGraph.records = Array.isArray(message.records) ? message.records : [];

  //-- The device reports the real size of its rolling table (twice the
  // baseline when its heap carried it): the plot always uses its pitch.
  if (Number.isFinite(message.capacity) && (message.capacity > 0))
  {
    ACTUAL_GRAPH.capacity = Math.round(message.capacity);
  }

  if (actualGraph.records.length === 0)
  {
    setActualGraphState("No measurements retained yet (is a telegram arriving?).");
  }
  else
  {
    setActualGraphState(actualGraph.records.length + " of " + ACTUAL_GRAPH.capacity +
                        " measurements plotted · /api/v3/actualgraph");
  }

  drawActualGraph();
}

// The six checkboxes of the toolbar: which phase series currently take
// part in the stacked bars and the scale.
function actualGraphVisible()
{
  const visible = {used: [true, true, true], returned: [true, true, true]};

  el.actualGraphChecks.querySelectorAll('input[type="checkbox"]').forEach((box) =>
  {
    const series = box.dataset.series;
    const phase = Number(box.dataset.phase);

    if ((series in visible) && (phase >= 0) && (phase < 3))
    {
      visible[series][phase] = box.checked;
    }
  });

  return visible;
}

function drawActualGraph()
{
  const canvas = el.actualGraphCanvas;

  if (canvas === null)
  {
    return;
  }

  const wrap = canvas.parentElement;
  const cssWidth = Math.max(320, wrap.clientWidth - 16);
  const cssHeight = ACTUAL_GRAPH.legendHeight + ACTUAL_GRAPH.panelHeight + ACTUAL_GRAPH.labelsHeight +
                    ACTUAL_GRAPH.panelGap + ACTUAL_GRAPH.legendHeight +
                    ACTUAL_GRAPH.gasPanelHeight + ACTUAL_GRAPH.labelsHeight + ACTUAL_GRAPH.bottomPad;
  const dpr = window.devicePixelRatio || 1;

  canvas.width = Math.round(cssWidth * dpr);
  canvas.height = Math.round(cssHeight * dpr);
  canvas.style.width = cssWidth + "px";
  canvas.style.height = cssHeight + "px";

  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cssWidth, cssHeight);

  const records = actualGraph.records;
  const visible = actualGraphVisible();
  const plotLeft = ACTUAL_GRAPH.marginLeft;
  const plotWidth = Math.max(10, cssWidth - plotLeft - ACTUAL_GRAPH.marginRight);

  // Steady capacity-slot pitch (the device's table size): the newest
  // sample always sits in the rightmost slot, so a filling table grows in
  // from the right edge and a full one shifts left with every new sample.
  const slot = plotWidth / ACTUAL_GRAPH.capacity;
  const firstSlot = ACTUAL_GRAPH.capacity - records.length;

  const panel1 =
  {
    y: ACTUAL_GRAPH.legendHeight,
    height: ACTUAL_GRAPH.panelHeight,
  };
  const panel2 =
  {
    y: panel1.y + panel1.height + ACTUAL_GRAPH.labelsHeight + ACTUAL_GRAPH.panelGap +
       ACTUAL_GRAPH.legendHeight,
    height: ACTUAL_GRAPH.gasPanelHeight,
  };

  let maxUsed = 0;
  let maxReturned = 0;
  let maxGas = 0;

  records.forEach((record) =>
  {
    const used = Array.isArray(record.used) ? record.used : [];
    const returned = Array.isArray(record.returned) ? record.returned : [];
    let usedSum = 0;
    let returnedSum = 0;

    for (let phase = 0; phase < 3; phase++)
    {
      if (visible.used[phase])
      {
        usedSum += Math.max(0, used[phase] || 0);
      }

      if (visible.returned[phase])
      {
        returnedSum += Math.max(0, returned[phase] || 0);
      }
    }

    maxUsed = Math.max(maxUsed, usedSum);
    maxReturned = Math.max(maxReturned, returnedSum);
    maxGas = Math.max(maxGas, record.gas || 0);
  });

  const wattStep = graphStep(Math.max(maxUsed, maxReturned), WATT_STEPS, 100);
  const wattTop = (maxUsed > 0) ? graphScale(maxUsed, wattStep) : 0;
  const wattBottom = (maxReturned > 0) ? -graphScale(maxReturned, wattStep) : 0;

  // An empty panel still needs a scale to sit its zero line on.
  if ((wattTop === 0) && (wattBottom === 0))
  {
    wattTop = wattStep;
  }
  const gasStep = graphStep(maxGas, KWH_STEPS, 1);
  const gasMax = graphScale(maxGas, gasStep);

  //-- Upper panel: one bar per sample in which every phase keeps its own
  // colour and its own slab — the used watts stack upwards from the zero
  // line (light L1 at the bottom, mid L2, dark L3 on top) and the returned
  // watts stack downwards the same way; a white hairline at each phase
  // boundary keeps neighbouring slabs clearly separated.
  const yOfEnergy = drawGraphPanelFrame(ctx, panel1, wattStep, wattTop, wattBottom, "W",
                                        [{color: ACTUAL_COLORS.used[0], label: "Used L1"},
                                         {color: ACTUAL_COLORS.used[1], label: "Used L2"},
                                         {color: ACTUAL_COLORS.used[2], label: "Used L3"},
                                         {color: ACTUAL_COLORS.returned[0], label: "Returned L1"},
                                         {color: ACTUAL_COLORS.returned[1], label: "Returned L2"},
                                         {color: ACTUAL_COLORS.returned[2], label: "Returned L3"}],
                                        plotLeft, plotWidth);

  const barWidth = Math.max(2, slot * 0.7);

  records.forEach((record, index) =>
  {
    const centre = plotLeft + (firstSlot + index + 0.5) * slot;
    const barX = centre - barWidth / 2;
    const used = Array.isArray(record.used) ? record.used : [];
    const returned = Array.isArray(record.returned) ? record.returned : [];
    let stackedUsed = 0;
    let stackedReturned = 0;

    for (let phase = 0; phase < 3; phase++)
    {
      const usedValue = visible.used[phase] ? Math.max(0, used[phase] || 0) : 0;

      if (usedValue > 0)
      {
        ctx.fillStyle = ACTUAL_COLORS.used[phase];
        ctx.globalAlpha = 0.9;
        ctx.fillRect(barX, yOfEnergy(stackedUsed + usedValue), barWidth,
                     yOfEnergy(stackedUsed) - yOfEnergy(stackedUsed + usedValue));
        ctx.globalAlpha = 1;

        //-- Hairline on the boundary to the phase stacked below, so each
        // phase slab of the bar reads as its own bar.
        if (stackedUsed > 0)
        {
          ctx.fillStyle = "rgba(255, 255, 255, 0.85)";
          ctx.fillRect(barX, yOfEnergy(stackedUsed) - 0.5, barWidth, 1);
        }

        stackedUsed += usedValue;
      }

      const returnedValue = visible.returned[phase] ? Math.max(0, returned[phase] || 0) : 0;

      if (returnedValue > 0)
      {
        ctx.fillStyle = ACTUAL_COLORS.returned[phase];
        ctx.globalAlpha = 0.9;
        ctx.fillRect(barX, yOfEnergy(-stackedReturned), barWidth,
                     yOfEnergy(-(stackedReturned + returnedValue)) - yOfEnergy(-stackedReturned));
        ctx.globalAlpha = 1;

        //-- Hairline on the boundary to the phase stacked further down.
        if (stackedReturned > 0)
        {
          ctx.fillStyle = "rgba(255, 255, 255, 0.85)";
          ctx.fillRect(barX, yOfEnergy(-stackedReturned) - 0.5, barWidth, 1);
        }

        stackedReturned += returnedValue;
      }
    }
  });

  drawGraphTimeLabels(ctx, records, panel1.y + panel1.height, slot, plotLeft,
                      (record) => record.time || "--:--:--", firstSlot);

  //-- Lower panel: the held gas rate as a blue line.
  const yOfGas = drawGraphPanelFrame(ctx, panel2, gasStep, gasMax, 0, "m³/h",
                                     [{color: ACTUAL_COLORS.gas, label: "Gas used"}],
                                     plotLeft, plotWidth);

  if (records.length > 0)
  {
    drawGraphLine(ctx, records.map((record) => record.gas || 0), firstSlot, slot, plotLeft,
                  yOfGas, ACTUAL_COLORS.gas);
  }

  drawGraphTimeLabels(ctx, records, panel2.y + panel2.height, slot, plotLeft,
                      (record) => record.time || "--:--:--", firstSlot);

  actualGraph.layout =
  {
    records: records,
    slot: slot,
    firstSlot: firstSlot,
    plotLeft: plotLeft,
    cssWidth: cssWidth,
    panelEnergy: panel1,
    panelGas: panel2,
    panelSplitY: (panel1.y + panel1.height + panel2.y) / 2,
    zeroY: yOfEnergy(0),
    visible: visible,
  };
  updateActualGraphTooltip();
}

// Hover popup: one chip over the hovered slot. The energy panel splits at
// its zero line: hovering the red (delivered) half shows the delivered
// watts only, the green (returned) half the returned watts only — always
// for the phases the checkboxes keep visible — and hovering the gas panel
// below shows the gas m³/h only.
function updateActualGraphTooltip()
{
  const canvas = el.actualGraphCanvas;

  if (canvas === null)
  {
    return;
  }

  if (actualGraph.tip === null)
  {
    actualGraph.tip = document.createElement("div");
    actualGraph.tip.className = "hoursGraphTooltip";
    actualGraph.tip.style.whiteSpace = "pre";
    actualGraph.tip.hidden = true;
    canvas.parentElement.appendChild(actualGraph.tip);
  }

  const tip = actualGraph.tip;
  const layout = actualGraph.layout;

  if ((layout === null) || (layout.records.length === 0) ||
      (actualGraph.hoverX === null) || (actualGraph.hoverY === null))
  {
    tip.hidden = true;
    return;
  }

  const index = Math.min(layout.records.length - 1,
                         Math.max(0, Math.floor((actualGraph.hoverX - layout.plotLeft) / layout.slot) -
                                       layout.firstSlot));
  const record = layout.records[index];
  const used = Array.isArray(record.used) ? record.used : [];
  const returned = Array.isArray(record.returned) ? record.returned : [];
  const lines = [record.time || "--:--:--"];
  const inGasPanel = (actualGraph.hoverY >= layout.panelSplitY);

  if (inGasPanel)
  {
    lines.push("gas  m³/h  " + (record.gas || 0).toFixed(3));
  }
  else
  {
    // One series per half of the energy panel: above the zero line the
    // cursor reads the stacked delivered (red) bar, below it the stacked
    // returned (green) one; the line still lists the visible phases only.
    const aboveZero = (actualGraph.hoverY < layout.zeroY);
    const series = aboveZero ? "used" : "returned";
    const watts = aboveZero ? used : returned;
    const parts = [];

    for (let phase = 0; phase < 3; phase++)
    {
      if (layout.visible[series][phase])
      {
        parts.push("L" + (phase + 1) + " " + ((watts[phase] || 0) / 1000).toFixed(2));
      }
    }

    if (parts.length === 0)
    {
      //-- That half's series are all switched off: nothing to read there.
      tip.hidden = true;
      return;
    }

    lines.push((aboveZero ? "Delivered kW  " : "Returned kW ") + parts.join("  "));
  }

  tip.textContent = lines.join("\n");
  tip.hidden = false;

  const centre = layout.plotLeft + (layout.firstSlot + index + 0.5) * layout.slot;
  const half = tip.offsetWidth / 2;
  const left = Math.min(layout.cssWidth - half, Math.max(half, centre));
  const panel = inGasPanel ? layout.panelGas : layout.panelEnergy;
  tip.style.left = (canvas.offsetLeft + left) + "px";
  tip.style.top = (canvas.offsetTop + panel.y + 2) + "px";
}

// The realtime table refreshes at least every five seconds; with a shorter
// read interval it follows that cadence instead (same timer rules as the
// Phase Load view, own handle so both views poll independently).
function updateActualGraphPolling()
{
  clearTimeout(actualGraphTimer);
  actualGraphTimer = null;

  if (state.screen !== "actualGraph")
  {
    return;
  }

  actualGraphTimer = setTimeout(() =>
  {
    wsSend("cmd=actualgraph");
    updateActualGraphPolling();
  }, powerPollDelayMs());
}

//-- --------------------------------------------------------------------------
//-- API view
//-- --------------------------------------------------------------------------

function setApiState(message)
{
  el.apiState.textContent = message;
}

// Self-contained styling for the result tab (see openApiResult): the tab
// is its own blank document, so it cannot rely on style.css; the palette
// follows the SPA (light iOS look, dark mode variant) and the JSON token
// colours are used by appendHighlightedJson.
const API_RESULT_CSS =
  "body{margin:0;background:#f2f2f7;color:#1c1c1e;" +
  "font-family:-apple-system,'Segoe UI',Helvetica,Arial,sans-serif;}" +
  "header{padding:14px 18px;background:#fff;border-bottom:1px solid #d1d1d6;" +
  "font-size:0.95rem;font-weight:600;word-break:break-all;}" +
  "main{padding:16px 18px;}" +
  "pre{margin:0;font-family:ui-monospace,Menlo,Consolas,monospace;" +
  "font-size:0.85rem;white-space:pre-wrap;word-break:break-word;}" +
  ".json .key{color:#0a3d91}.json .str{color:#0a7d2c}.json .num{color:#7a3eb5}" +
  ".json .bool{color:#c2410c}.json .nullt{color:#6e6e73;font-style:italic}" +
  ".error{color:#c0392b;font-weight:600}" +
  "@media (prefers-color-scheme:dark){body{background:#1c1c1e;color:#f2f2f7}" +
  "header{background:#2c2c2e;border-bottom-color:#3a3a3c}" +
  ".json .key{color:#7fb3ff}.json .str{color:#6fe08a}.json .num{color:#c9a0ff}" +
  ".json .bool{color:#ffb366}.json .nullt{color:#9a9aa0}.error{color:#ff6961}}";

// Appends an already indented JSON document to a <pre> in the result tab,
// one coloured <span> per token (key, string, number, boolean, null). The
// text only ever travels through textContent — like every other rendering
// path in this app, so telegram bytes are never interpreted as HTML.
function appendHighlightedJson(doc, pre, text)
{
  const tokenPattern =
    /("(\\u[a-zA-Z0-9]{4}|\\[^u]|[^\\"])*"(\s*:)?|\b(?:true|false)\b|\bnull\b|-?\d+(?:\.\d*)?(?:[eE][+-]?\d+)?)/g;

  let lastEnd = 0;
  let match = tokenPattern.exec(text);

  while (match !== null)
  {
    if (match.index > lastEnd)
    {
      pre.appendChild(doc.createTextNode(text.slice(lastEnd, match.index)));
    }

    const token = doc.createElement("span");
    let cls = "num";

    if (match[0].startsWith("\""))
    {
      cls = match[0].trimEnd().endsWith(":") ? "key" : "str";
    }
    else if ((match[0] === "true") || (match[0] === "false"))
    {
      cls = "bool";
    }
    else if (match[0] === "null")
    {
      cls = "nullt";
    }

    token.className = cls;
    token.textContent = match[0];
    pre.appendChild(token);

    lastEnd = tokenPattern.lastIndex;
    match = tokenPattern.exec(text);
  }

  if (lastEnd < text.length)
  {
    pre.appendChild(doc.createTextNode(text.slice(lastEnd)));
  }
}

// Opens the result of one GET endpoint in a new browser tab as a readable
// page: the tab is created inside the click gesture (a later window.open
// after the await would be popup-blocked), filled once the fetch settles.
// JSON answers arrive indented and token-coloured; the plain-text (raw
// telegram) and csv answers keep their exact bytes in a <pre>.
function openApiResult(url)
{
  const tab = window.open("", "_blank");

  if (tab === null)
  {
    setApiState("The browser blocked the result tab: allow pop-ups for this page.");
    return;
  }

  const doc = tab.document;
  doc.title = url;

  const style = doc.createElement("style");
  style.textContent = API_RESULT_CSS;

  const header = doc.createElement("header");
  header.textContent = "Loading " + url + " ...";

  const main = doc.createElement("main");

  doc.head.appendChild(style);
  doc.body.appendChild(header);
  doc.body.appendChild(main);

  fetch(url, {cache: "no-store"})
    .then((response) =>
    {
      const contentType = response.headers.get("content-type") || "";

      return response.text().then((text) =>
      {
        return {status: response.status, contentType: contentType, text: text};
      });
    })
    .then((result) =>
    {
      header.textContent = url + "  —  HTTP " + result.status;
      main.textContent = "";

      const pre = doc.createElement("pre");

      if (result.contentType.indexOf("application/json") >= 0)
      {
        let formatted = result.text;

        try
        {
          formatted = JSON.stringify(JSON.parse(result.text), null, 2);
        }
        catch (error)
        {
          //-- Not parseable after all: show the bytes as they arrived.
        }

        pre.className = "json";
        appendHighlightedJson(doc, pre, formatted);
      }
      else
      {
        pre.textContent = result.text;
      }

      main.appendChild(pre);
    })
    .catch((error) =>
    {
      header.textContent = url;
      main.textContent = "Could not load the endpoint: " + error.message;
      main.className = "error";
    });
}

// Turns a listed endpoint URI into one a browser tab can actually open:
// templated entries drop their placeholders, and the device then answers
// with its documented defaults (the newest accepted telegram, the hours
// ring), so every GET row is openable as listed.
function apiOpenUrl(uri)
{
  let url = String(uri);
  const queryStart = url.indexOf("?");

  if (queryStart >= 0)
  {
    url = url.slice(0, queryStart);
  }

  const placeholderStart = url.indexOf("<");

  if (placeholderStart >= 0)
  {
    url = url.slice(0, placeholderStart);
  }

  if (url.endsWith("/"))
  {
    url = url.slice(0, -1);
  }

  return url;
}

// Fetches the device's own published index (GET /api/v3, served straight
// from the same table the console and handlers use), so the documentation
// can never drift from the endpoints that are really registered.
function refreshApiList()
{
  setApiState("Loading the endpoint index from /api/v3...");

  fetch("/api/v3", {cache: "no-store"})
    .then((response) =>
    {
      if (!response.ok)
      {
        throw new Error("HTTP " + response.status);
      }

      return response.json();
    })
    .then((data) =>
    {
      renderApiList(data);
    })
    .catch((error) =>
    {
      setApiState("Could not load the endpoint index: " + error.message + ".");
    });
}

// Builds one row per endpoint, mirroring the classic API documentation
// page: a blue button with the URI on the left and the description beside
// it. A GET button opens its result in a new browser tab; POST entries
// cannot be opened that way, so their button is greyed out and the
// description names the method. All text goes in through textContent.
function renderApiList(data)
{
  el.apiList.textContent = "";

  const endpoints = (data && Array.isArray(data.endpoints)) ? data.endpoints : [];

  if (endpoints.length === 0)
  {
    setApiState("The device reported no endpoints.");
    return;
  }

  endpoints.forEach((endpoint) =>
  {
    const row = document.createElement("div");
    row.className = "apiRow";

    const button = document.createElement("button");
    button.type = "button";
    button.className = "apiButton";
    button.textContent = endpoint.uri;

    if (endpoint.method === "GET")
    {
      const openUrl = apiOpenUrl(endpoint.uri);
      button.title = "Open " + openUrl + " in a new browser tab";
      button.addEventListener("click", () =>
      {
        openApiResult(openUrl);
      });
    }
    else
    {
      button.disabled = true;
      button.title = endpoint.method + " endpoint: it cannot be opened in a browser tab";
    }

    const description = document.createElement("span");
    description.className = "apiDescription";
    description.textContent = (endpoint.method === "GET")
      ? endpoint.description
      : "[" + endpoint.method + "] " + endpoint.description;

    row.appendChild(button);
    row.appendChild(description);
    el.apiList.appendChild(row);
  });

  setApiState(endpoints.length + " endpoints published under /api/v3: a GET button " +
              "opens its result in a new browser tab.");
}

//-- --------------------------------------------------------------------------
//-- Tariffs view
//-- --------------------------------------------------------------------------

function setTariffsState(message)
{
  el.tariffsState.textContent = message;
}

// Formats a Date as the "YYYY-MM-DDTHH:MM:SSZ" the tariff query API
// expects for its from and to parameters (always UTC, whole seconds).
function toUtcIsoText(date)
{
  const pad = (value) => String(value).padStart(2, "0");

  return date.getUTCFullYear() + "-" + pad(date.getUTCMonth() + 1) + "-" + pad(date.getUTCDate()) +
         "T" + pad(date.getUTCHours()) + ":" + pad(date.getUTCMinutes()) + ":" +
         pad(date.getUTCSeconds()) + "Z";
}

// Formats one tariff boundary the way the device prints its wall clock
// ("DD-MM-YYYY HH:MM") in the browser's local time zone, so a quarter-hour
// price reads as the local instant it applies to. Text that does not parse
// as a date is shown verbatim instead of blanking the cell.
function formatTariffDateTime(isoText)
{
  const date = new Date(isoText);

  if (!Number.isFinite(date.getTime()))
  {
    return String(isoText);
  }

  const pad = (value) => String(value).padStart(2, "0");

  return pad(date.getDate()) + "-" + pad(date.getMonth() + 1) + "-" + date.getFullYear() +
         " " + pad(date.getHours()) + ":" + pad(date.getMinutes());
}

// Reads the stored schedule from GET /api/v3/tariffs with resolution=raw
// (the same answer the API view can open in a tab): the query window starts
// a week back so an interval that covers the present but started earlier
// (a gas day spanning many quarter-hours) is never cut off, and runs a
// year ahead so it reaches the last stored interval whatever the supplier
// posted. The walk returns every interval overlapping the window; the
// render pass keeps only those that still stand (their end lies ahead of
// now), so expired records never show. Plain HTTP like the API view: no
// WebSocket command is involved.
function refreshTariffs()
{
  setTariffsState("Reading the tariff schedule from /api/v3/tariffs...");

  const nowMs = Date.now();
  const from = new Date(nowMs - 7 * 24 * 60 * 60 * 1000);
  const to = new Date(nowMs + 366 * 24 * 60 * 60 * 1000);
  const url = "/api/v3/tariffs?from=" + toUtcIsoText(from) +
              "&to=" + toUtcIsoText(to) + "&resolution=raw";

  fetch(url, {cache: "no-store"})
    .then((response) =>
    {
      // The device answers refusals with a small {"ok":false,"error":...}
      // document: its reason is shown instead of the bare status code.
      return response.json().then((data) =>
      {
        if (!response.ok)
        {
          throw new Error(data.error || ("HTTP " + response.status));
        }

        return data;
      });
    })
    .then((data) =>
    {
      renderTariffTable(data);
    })
    .catch((error) =>
    {
      setTariffsState("Could not read the tariff schedule: " + error.message + ".");
    });
}

// Builds the tariff table: one row per still-standing interval (end after
// now), oldest first — the row that covers the present moment carries the
// tariffNowRow highlight. All text goes in through textContent.
function renderTariffTable(data)
{
  const tbody = el.tariffsTable.tBodies[0];
  tbody.textContent = "";

  const intervals = (data && Array.isArray(data.intervals)) ? data.intervals : [];
  const currency = (data && data.currency) ? data.currency : "EUR";
  const nowMs = Date.now();

  const standing = intervals.filter((interval) =>
  {
    const endMs = Date.parse(interval.end);
    return Number.isFinite(endMs) && (endMs > nowMs);
  });

  if (standing.length === 0)
  {
    setTariffsState("No tariff interval is in use or coming up " +
                    "(expired records are not shown).");
    return;
  }

  standing.forEach((interval) =>
  {
    const startMs = Date.parse(interval.start);
    const endMs = Date.parse(interval.end);

    const row = document.createElement("tr");

    if (Number.isFinite(startMs) && Number.isFinite(endMs) &&
        (startMs <= nowMs) && (nowMs < endMs))
    {
      row.className = "tariffNowRow";
    }

    const cells = [
      formatTariffDateTime(interval.start),
      formatTariffDateTime(interval.end),
      graphCellNumber(interval.importPricePerKwh, 4),
      graphCellNumber(interval.exportPricePerKwh, 4),
      graphCellNumber(interval.gasPricePerM3, 4),
    ];

    cells.forEach((text, index) =>
    {
      const cell = document.createElement("td");
      cell.textContent = text;

      // The price columns align right so the digits of successive
      // intervals line up; the two time columns keep the default left.
      if (index > 1)
      {
        cell.className = "valueNumeric";
      }

      row.appendChild(cell);
    });

    tbody.appendChild(row);
  });

  setTariffsState(standing.length + ((standing.length === 1) ? " interval" : " intervals") +
                  " in use or coming up · prices in " + currency +
                  " per kWh (delivered and returned) and per m³ (gas) · read at " +
                  new Date().toLocaleTimeString("en-GB"));
}

//-- --------------------------------------------------------------------------
//-- View switching and boot
//-- --------------------------------------------------------------------------

// Builds the segmented control from the VIEWS registry for the active bar:
// the logger bar shows the logger views and ends with [System], the system
// bar shows the system views (Settings, Show RingFile, Tariffs, API) and
// ends with [Back]. Every button carries the same .tab pill styling, so the
// look and feel of the bar stays as it was and every future feature button
// gets the styling and click wiring for free.
function buildTabBar()
{
  // The macOS-style bar caption names the active bar left of the pills:
  // "Logger:" before the feature buttons and "System:" before the settings
  // buttons (the span in the .tabRow of index.html).
  el.tabBarCaption.textContent = (state.bar === "logger") ? "Logger:" : "System:";

  el.tabBar.textContent = "";
  Object.keys(tabButtons).forEach((id) =>
  {
    delete tabButtons[id];
  });

  VIEWS.filter((view) => view.group === state.bar).forEach((view) =>
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

  // The bar switch sits at the right end of every bar: [System] opens the
  // system bar, [Back] returns to the logger screen last shown.
  const switchButton = document.createElement("button");
  switchButton.type = "button";
  switchButton.className = "tab";
  switchButton.textContent = (state.bar === "logger") ? "System" : "Back";
  switchButton.setAttribute("role", "tab");
  switchButton.addEventListener("click", () => switchBar());
  el.tabBar.appendChild(switchButton);

  markActiveTab();
}

// Marks the button of the shown view as the active tab; only the buttons of
// the active bar live in the bar, so the map is walked.
function markActiveTab()
{
  Object.keys(tabButtons).forEach((id) =>
  {
    tabButtons[id].classList.toggle("active", id === state.screen);
    tabButtons[id].setAttribute("aria-selected", String(id === state.screen));
  });
}

// Swaps the two bars: [System] switches to the system bar and opens its
// first view, [Back] switches back to the logger bar and restores the
// logger screen last shown before the system bar opened.
function switchBar()
{
  if (state.bar === "logger")
  {
    const firstSystemView = VIEWS.find((view) => view.group === "system");

    state.bar = "system";
    buildTabBar();
    showScreen(firstSystemView.id);
    return;
  }

  state.bar = "logger";
  buildTabBar();
  showScreen(state.lastLoggerView);
}

function showScreen(name)
{
  const view = viewById(name);

  if (view === undefined)
  {
    return;
  }

  state.screen = view.id;

  // The Back button restores this screen once the system bar opened.
  if (view.group === "logger")
  {
    state.lastLoggerView = view.id;
  }

  VIEWS.forEach((entry) =>
  {
    el[entry.section].hidden = (entry.id !== view.id);
  });

  markActiveTab();

  if (view.command !== null)
  {
    wsSend(viewCommand(view));
  }

  // Views backed by a plain HTTP source (the API view fetches its endpoint
  // index) reload through their refresh hook instead of a WebSocket command.
  if (typeof view.refresh === "function")
  {
    view.refresh();
  }

  reportActiveScreen();
  updatePowerPolling();
  updateActualGraphPolling();
}

function boot()
{
  bindElements();
  buildTabBar();
  el.settingsForm.addEventListener("submit", saveSettings);

  // The device never sends the stored MQTT password: the save only carries
  // the mqttPassword field once the operator has typed into this field
  // (state.mqttPasswordChanged), so an untouched field keeps the password.
  el.inpMqttPassword.addEventListener("input", () =>
  {
    state.mqttPasswordChanged = true;
  });

  el.srcToggle.addEventListener("change", updateSourceLabels);
  el.rxPolarityToggle.addEventListener("change", updatePolarityLabels);
  el.txPolarityToggle.addEventListener("change", updateTxPolarityLabels);
  el.displayRotationToggle.addEventListener("change", updateRotationLabels);
  el.inpRfsSync.addEventListener("input", updateRfsWearEstimate);

  // The settings section picker: one radio button per group shows only the
  // fields of that group; the Save button below the groups stays visible on
  // every section.
  document.querySelectorAll('input[name="settingsSection"]').forEach((radio) =>
  {
    radio.addEventListener("change", () =>
    {
      showSettingsGroup(radio.value);
    });
  });

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

  // The ring records follow the latest telegram and the view does not poll:
  // this button re-requests the newest frame of the selected ring on demand,
  // and picking another radio button fetches that ring immediately.
  el.refreshRingFile.addEventListener("click", () =>
  {
    sendRingFile();
  });

  document.querySelectorAll('input[name="ringFileChoice"]').forEach((radio) =>
  {
    radio.addEventListener("change", () =>
    {
      sendRingFile();
    });
  });

  // The period records follow the ring: the period views do not poll, these
  // buttons re-request the usage series on demand (switching to a view
  // requests it as well, like every other view command). The presentation
  // radio group switches the same cached frame between canvas and table, so
  // it only redraws without asking the device again.
  Object.keys(PERIOD_GRAPHS).forEach((viewId) =>
  {
    el["refresh" + viewId.charAt(0).toUpperCase() + viewId.slice(1)]
      .addEventListener("click", () =>
      {
        wsSend("cmd=graph&ring=" + PERIOD_GRAPHS[viewId].ring);
      });

    document.querySelectorAll('input[name="' + viewId + 'Mode"]').forEach((radio) =>
    {
      radio.addEventListener("change", () =>
      {
        // The radio values are the modes themselves: graph, table and
        // (months view only) costs; anything unexpected falls back to the
        // canvas.
        periodGraphs[viewId].mode = (radio.value === "table") ? "table"
                                        : ((radio.value === "costs") ? "costs" : "graph");
        showPeriodGraphView(viewId);
      });
    });
  });

  // The Actual Graph polls while it is open; this button re-requests the
  // rolling per-phase table right away, and the six phase checkboxes
  // restyle the stacked series (and the scale) without a device round trip.
  el.refreshActualGraph.addEventListener("click", () =>
  {
    wsSend("cmd=actualgraph");
  });

  el.actualGraphChecks.addEventListener("change", () =>
  {
    drawActualGraph();
  });

  // The tariff table is read on demand (and when the view opens): tariff
  // batches arrive at their own pace, so the view does not poll and this
  // button re-reads the schedule from /api/v3/tariffs right away.
  el.refreshTariffs.addEventListener("click", () =>
  {
    refreshTariffs();
  });

  // Take over control again after another browser page evicted this page:
  // a reload claims control with the first frame of the fresh WebSocket
  // session.
  el.takeoverTakeBack.addEventListener("click", () =>
  {
    location.reload();
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

    if (state.screen in periodGraphs)
    {
      drawPeriodGraph(state.screen);
    }

    if (state.screen === "actualGraph")
    {
      drawActualGraph();
    }
  });

  // Hovering the history plot pops up the meter time and total watts of
  // the bar under the cursor; leaving the plot hides the popup again.
  el.powerSpark.addEventListener("mousemove", (event) =>
  {
    const rect = el.powerSpark.getBoundingClientRect();
    powerHoverX = event.clientX - rect.left;
    updatePowerTooltip();
  });

  el.powerSpark.addEventListener("mouseleave", () =>
  {
    powerHoverX = null;
    updatePowerTooltip();
  });

  // Hovering a graph pops up its chips for the slot under the cursor (the
  // period graphs separate chips for used, returned and gas, the Actual
  // Graph one per-phase chip); leaving the plot hides them again.
  Object.keys(periodGraphs).forEach((viewId) =>
  {
    const canvas = el[viewId + "Canvas"];

    canvas.addEventListener("mousemove", (event) =>
    {
      const rect = canvas.getBoundingClientRect();
      periodGraphs[viewId].hoverX = event.clientX - rect.left;
      periodGraphs[viewId].hoverY = event.clientY - rect.top;
      updatePeriodGraphTooltip(viewId);
    });

    canvas.addEventListener("mouseleave", () =>
    {
      periodGraphs[viewId].hoverX = null;
      periodGraphs[viewId].hoverY = null;
      updatePeriodGraphTooltip(viewId);
    });
  });

  el.actualGraphCanvas.addEventListener("mousemove", (event) =>
  {
    const rect = el.actualGraphCanvas.getBoundingClientRect();
    actualGraph.hoverX = event.clientX - rect.left;
    actualGraph.hoverY = event.clientY - rect.top;
    updateActualGraphTooltip();
  });

  el.actualGraphCanvas.addEventListener("mouseleave", () =>
  {
    actualGraph.hoverX = null;
    actualGraph.hoverY = null;
    updateActualGraphTooltip();
  });

  wsConnect();
  // The realtime Phase Load view is the default screen at every start and
  // reload of the page (reset and startup).
  showScreen("power");
}

document.addEventListener("DOMContentLoaded", boot);
