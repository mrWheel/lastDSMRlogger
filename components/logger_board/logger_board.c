//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_board.c - on-device TFT debug lines and power panel
//-- Date: 2026-11-16 · Build: 10
//--
//-- Everything is drawn through the documented API of the esp32_s3_piggyback
//-- component: tft_ec11_init once, then filled rectangles and fixed-width
//-- text. The component inputs are started so a small poll task can act on
//-- the AUX button and the encoder. Drawing is serialized with a mutex
//-- because WiFi callbacks, the receive task and the HTTP handlers all
//-- report state.
//--
//-- Before the first accepted telegram the panel shows the four debug lines.
//-- When the power panel becomes active the screen is fully cleared first:
//-- a white title line with the telegram number left and the meter date/time
//-- right aligned on it, then one row per phase plus Totaal with a header
//-- band, a centre-zero signed watt
//-- bar (consumption red, feed-in green) with its integer scale labels
//-- inside the bar and the watt value.
//-- From the graphics panel a short AUX press opens the power text screen
//-- (delivered and returned watts of the phase the encoder selects) and a
//-- medium-long press swaps the bars for the system information view; a
//-- short press returns to the graphics panel.
//-- ==========================================================================

#include "logger_board.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp32_s3_piggyback.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *tag = "loggerBoard";

//-- Scale 2 gives a 12 by 16 pixel cell: 25 characters fit on the 320 pixel
//-- wide panel with a small left margin.
#define BOARD_FONT_SCALE 2
#define BOARD_CHARS_PER_LINE 25
#define BOARD_LEFT_X 4
#define BOARD_LINE_PITCH (8 * BOARD_FONT_SCALE)

#define BOARD_LINE_PARSER 1
#define BOARD_LINE_SCREEN 2
#define BOARD_LINE_P1 3
#define BOARD_LINE_WIFI 4
#define BOARD_LINE_IP 5

#define BOARD_TEXT_MAX 48

//-- --------------------------------------------------------------------------
//-- Power panel geometry (layout adapts to the panel orientation reported
//-- by the component: 320x240 landscape or 240x320 portrait)
//-- --------------------------------------------------------------------------

//-- Title line (white, screen name left and the meter date/time right
//-- aligned on the bar screen) and a sub band below it (the selected phase
//-- on the text screen), then four equal rows.
//-- Within a row: header band and a centre-zero bar carrying its integer
//-- scale labels; the watt value sits at the bottom of the row.
#define PANEL_NAME_H 20
#define PANEL_SUB_H 16
#define PANEL_TITLE_H (PANEL_NAME_H + PANEL_SUB_H)
#define PANEL_HEADER_H 16
#define PANEL_BAR_H 16
#define PANEL_WATT_H 16
#define PANEL_HINT_H 20
#define PANEL_ROWS 4

#define PANEL_BAR_Y PANEL_HEADER_H
#define PANEL_EDGE_MARGIN 4

//-- All panel text uses the documented native faces of the component
//-- (SMALL 6x8, MEDIUM 8x12 and LARGE 9x16 cells; the advance includes the
//-- 1 px inter-character gap). Every text steps one size down from the
//-- previously stretched cells: the big titles and the delivered/returned
//-- values became MEDIUM and LARGE, every small label, value and stamp
//-- became SMALL.
#define FONT_TITLE TFT_EC11_FONT_MEDIUM
#define FONT_STAMP TFT_EC11_FONT_SMALL
#define FONT_VALUE TFT_EC11_FONT_SMALL
#define FONT_LABEL TFT_EC11_FONT_SMALL
#define FONT_WATT TFT_EC11_FONT_LARGE
#define FONT_INFO TFT_EC11_FONT_MEDIUM
#define FONT_DATA TFT_EC11_FONT_LARGE

//-- Bar scale ladder: the smallest step that fits the largest absolute
//-- watt of the frame; frames above 3800 W stay on the 3800 W step.
//-- Published for the SPA realtime view so its bars scale identically to
//-- the panel (logger_board.h).
const uint16_t loggerPowerScales[LOGGER_POWER_SCALE_COUNT] = {500u, 1000u, 1700u, 2400u, 3100u, 3800u};

//-- Panel colours (RGB565).
#define PANEL_HEADER_COLOR TFT_EC11_RGB565(43, 60, 94)
#define PANEL_BAR_BG TFT_EC11_RGB565(232, 232, 236)
#define PANEL_TICK_COLOR TFT_EC11_RGB565(90, 90, 98)
#define PANEL_CONSUME_COLOR TFT_EC11_RGB565(255, 59, 48)
#define PANEL_PRODUCE_COLOR TFT_EC11_RGB565(34, 160, 60)

//-- Screen modes after the first accepted telegram: the graphics panel,
//-- the AUX power text screen and the AUX medium-press system view.
typedef enum
{
  BOARD_MODE_GRAPHICS,
  BOARD_MODE_POWER_TEXT,
  BOARD_MODE_SYSTEM
} BoardMode;

#define BOARD_INPUT_QUEUE_LEN 16u

//-- Landscape rotations of the component: the assembled orientation is the
//-- Kconfig default (3); rotation 1 is that same landscape turned 180
//-- degrees, so the flip only swaps the vertical mirror.
#define BOARD_ROTATION_NORMAL 3u
#define BOARD_ROTATION_UPSIDE_DOWN 1u

typedef struct
{
  bool available;
  bool panelActive;
  SemaphoreHandle_t mutex;
  BoardMode mode;
  uint8_t powerIndex;
  loggerPowerSnapshot panel;
  char parserVersion[BOARD_TEXT_MAX];
  char screen[BOARD_TEXT_MAX];
  char p1Note[BOARD_TEXT_MAX];
  char wifiStatus[BOARD_TEXT_MAX];
  char wifiIp[BOARD_TEXT_MAX];
  char ssid[BOARD_TEXT_MAX];
  char ip[BOARD_TEXT_MAX];
} LoggerBoard;

static LoggerBoard board;

static const char *const phaseLabels[PANEL_ROWS] = {"Fase 1", "Fase 2", "Fase 3", "Totaal"};

static void drawLine(uint8_t row, const char *label, const char *value)
{
  char line[BOARD_CHARS_PER_LINE + 1];
  snprintf(line, sizeof line, "%s%s", label, value);

  tft_ec11_draw_text(BOARD_LEFT_X, (int)(row * BOARD_LINE_PITCH), BOARD_CHARS_PER_LINE, line);
}

static void refreshDebugLines(void)
{
  drawLine(BOARD_LINE_PARSER, "parser : ", board.parserVersion);
  drawLine(BOARD_LINE_SCREEN, "screen : ", board.screen);
  drawLine(BOARD_LINE_P1, "p1     : ", board.p1Note);
  drawLine(BOARD_LINE_WIFI, "wifi   : ", board.wifiStatus);
  drawLine(BOARD_LINE_IP, "ip     : ", board.wifiIp);
}

//-- --------------------------------------------------------------------------
//-- Power panel drawing
//-- --------------------------------------------------------------------------

//-- True colour for a signed watt value: consumption red, feed-in green.
static uint16_t signedValueColor(float value)
{
  if (value < 0.0f)
  {
    return PANEL_CONSUME_COLOR;
  }

  if (value > 0.0f)
  {
    return PANEL_PRODUCE_COLOR;
  }

  return TFT_EC11_WHITE;
}

//-- Smallest ladder step that fits the largest absolute watt, so the bar
//-- scale follows the size of the consumption and feed-in of the frame.
//-- Also answers the scale for the SPA realtime view (logger_board.h).
uint16_t loggerBoardPickPowerScale(const loggerPowerSnapshot *snapshot)
{
  float maxAbs = 0.0f;
  const float values[PANEL_ROWS] = {snapshot->phaseWatts[0], snapshot->phaseWatts[1],
                                    snapshot->phaseWatts[2], snapshot->totalWatts};

  for (int index = 0; index < PANEL_ROWS; index++)
  {
    float magnitude = fabsf(values[index]);

    if (magnitude > maxAbs)
    {
      maxAbs = magnitude;
    }
  }

  for (size_t step = 0u; step < LOGGER_POWER_SCALE_COUNT; step++)
  {
    if (maxAbs <= (float)loggerPowerScales[step])
    {
      return loggerPowerScales[step];
    }
  }

  return loggerPowerScales[LOGGER_POWER_SCALE_COUNT - 1u];
}

//-- Cell metrics of the documented native faces: the advance carries the
//-- 1 px inter-character gap and the cell height is the cleared line height
//-- (SMALL 6x8, MEDIUM 8x12, LARGE 9x16 per the component README).
static int fontAdvance(tft_ec11_font_t font)
{
  if (font == TFT_EC11_FONT_LARGE)
  {
    return 9;
  }

  if (font == TFT_EC11_FONT_MEDIUM)
  {
    return 8;
  }

  return 6;
}

static int fontCellHeight(tft_ec11_font_t font)
{
  if (font == TFT_EC11_FONT_LARGE)
  {
    return 16;
  }

  if (font == TFT_EC11_FONT_MEDIUM)
  {
    return 12;
  }

  return 8;
}

static void drawLeftText(int y, const char *text, tft_ec11_font_t font, uint16_t background, uint16_t foreground)
{
  tft_ec11_draw_text_font(PANEL_EDGE_MARGIN, y, text, font, background, foreground);
}

static void drawCenteredText(int y, int panelWidth, const char *text, tft_ec11_font_t font, uint16_t background, uint16_t foreground)
{
  int x = (panelWidth - (int)strlen(text) * fontAdvance(font)) / 2;

  if (x < 0)
  {
    x = 0;
  }

  tft_ec11_draw_text_font(x, y, text, font, background, foreground);
}

static void drawRightAlignedText(int y, int panelWidth, const char *text, int rightMargin, tft_ec11_font_t font, uint16_t background, uint16_t foreground)
{
  int x = panelWidth - (int)strlen(text) * fontAdvance(font) - rightMargin;

  if (x < 0)
  {
    x = 0;
  }

  tft_ec11_draw_text_font(x, y, text, font, background, foreground);
}

//-- Every screen starts with a white title line (screen name left and an
//-- optional right aligned text such as the panel meter date/time) and a
//-- sub band below it carrying optional right aligned text.
static void drawScreenTitle(int panelWidth, const char *name, const char *topRightText, const char *rightText)
{
  tft_ec11_fill_rect(0, 0, panelWidth, PANEL_NAME_H, TFT_EC11_WHITE);
  tft_ec11_draw_text_font(PANEL_EDGE_MARGIN, (PANEL_NAME_H - fontCellHeight(FONT_TITLE)) / 2,
                          name, FONT_TITLE, TFT_EC11_WHITE, TFT_EC11_BLACK);

  if ((topRightText != NULL) && (topRightText[0] != '\0'))
  {
    drawRightAlignedText((PANEL_NAME_H - fontCellHeight(FONT_STAMP)) / 2, panelWidth, topRightText,
                         PANEL_EDGE_MARGIN, FONT_STAMP, TFT_EC11_WHITE, TFT_EC11_BLACK);
  }

  tft_ec11_fill_rect(0, PANEL_NAME_H, panelWidth, PANEL_SUB_H, PANEL_HEADER_COLOR);

  if ((rightText != NULL) && (rightText[0] != '\0'))
  {
    drawRightAlignedText(PANEL_NAME_H + (PANEL_SUB_H - fontCellHeight(FONT_STAMP)) / 2, panelWidth, rightText,
                         PANEL_EDGE_MARGIN, FONT_STAMP, PANEL_HEADER_COLOR, TFT_EC11_WHITE);
  }
}

static void drawBar(int y, int panelWidth, float watts, uint16_t scale)
{
  int mid = panelWidth / 2;
  int halfSpan = mid - PANEL_EDGE_MARGIN;
  char label[16];

  tft_ec11_fill_rect(0, y, panelWidth, PANEL_BAR_H, PANEL_BAR_BG);

  int half = (int)(fabsf(watts) * (float)halfSpan / (float)scale);

  if (half > halfSpan)
  {
    half = halfSpan;
  }

  if ((watts < 0.0f) && (half > 0))
  {
    //-- Consumption grows to the left of centre.
    tft_ec11_fill_rect(mid - half, y, half, PANEL_BAR_H, PANEL_CONSUME_COLOR);
  }
  else if ((watts > 0.0f) && (half > 0))
  {
    //-- Feed-in grows to the right of centre.
    tft_ec11_fill_rect(mid + 1, y, half, PANEL_BAR_H, PANEL_PRODUCE_COLOR);
  }

  //-- Scale labels as integers inside the bar so they sit in contrast on
  //-- the bar background and the coloured fill.
  snprintf(label, sizeof label, "-%" PRIu16, scale);
  tft_ec11_draw_text_font(2, y + (PANEL_BAR_H - fontCellHeight(FONT_VALUE)) / 2, label, FONT_VALUE, PANEL_BAR_BG, PANEL_TICK_COLOR);

  snprintf(label, sizeof label, "%" PRIu16, scale);
  drawRightAlignedText(y + (PANEL_BAR_H - fontCellHeight(FONT_VALUE)) / 2, panelWidth, label, 2, FONT_VALUE, PANEL_BAR_BG, PANEL_TICK_COLOR);

  drawCenteredText(y + (PANEL_BAR_H - fontCellHeight(FONT_VALUE)) / 2, panelWidth, "0", FONT_VALUE, PANEL_BAR_BG, PANEL_TICK_COLOR);

  //-- The centre tick stays on top so the zero label never covers it.
  tft_ec11_fill_rect(mid - 1, y, 2, PANEL_BAR_H, PANEL_TICK_COLOR);
}

static void drawPowerRow(int index, float watts, float amps, float volts, int y, int panelWidth, int rowHeight, uint16_t scale)
{
  char text[24];
  int labelTextY = y + (PANEL_HEADER_H - fontCellHeight(FONT_LABEL)) / 2;
  int valueTextY = y + (PANEL_HEADER_H - fontCellHeight(FONT_VALUE)) / 2;
  int wattY = y + rowHeight - PANEL_WATT_H;

  //-- Header band: phase current left, voltage right and the centred phase
  //-- label, all in the small native face.
  tft_ec11_fill_rect(0, y, panelWidth, PANEL_HEADER_H, PANEL_HEADER_COLOR);

  snprintf(text, sizeof text, "%.1fA", (double)amps);
  drawLeftText(valueTextY, text, FONT_VALUE, PANEL_HEADER_COLOR, TFT_EC11_WHITE);

  drawCenteredText(labelTextY, panelWidth, phaseLabels[index], FONT_LABEL, PANEL_HEADER_COLOR, TFT_EC11_WHITE);

  snprintf(text, sizeof text, "%.1fV", (double)volts);
  drawRightAlignedText(valueTextY, panelWidth, text, PANEL_EDGE_MARGIN, FONT_VALUE, PANEL_HEADER_COLOR, TFT_EC11_WHITE);

  drawBar(y + PANEL_BAR_Y, panelWidth, watts, scale);

  //-- Full-width clear behind the watt row so a previous wider label never
  //-- leaves ghost pixels behind.
  tft_ec11_fill_rect(0, wattY, panelWidth, PANEL_WATT_H, TFT_EC11_BLACK);
  snprintf(text, sizeof text, "%.1f Watt", (double)watts);
  drawCenteredText(wattY, panelWidth, text, FONT_WATT, TFT_EC11_BLACK, signedValueColor(watts));
}

static void drawPanel(void)
{
  const loggerPowerSnapshot *snapshot = &board.panel;
  uint16_t scale = loggerBoardPickPowerScale(snapshot);
  int width = (int)tft_ec11_width();
  int height = (int)tft_ec11_height();
  int rowHeight = (height - PANEL_TITLE_H) / PANEL_ROWS;
  char text[32];

  snprintf(text, sizeof text, "TELEGRAM %" PRIu32, snapshot->sequence);
  drawScreenTitle(width, text,
                  (snapshot->meterTime[0] != '\0') ? snapshot->meterTime : "no meter time", "");

  for (int row = 0; row < PANEL_ROWS; row++)
  {
    int y = PANEL_TITLE_H + row * rowHeight;
    float watts = (row < 3) ? snapshot->phaseWatts[row] : snapshot->totalWatts;
    float amps = (row < 3) ? snapshot->phaseAmps[row] : snapshot->totalAmps;
    float volts = (row < 3) ? snapshot->phaseVolts[row] : snapshot->totalVolts;

    drawPowerRow(row, watts, amps, volts, y, width, rowHeight, scale);
  }
}

//-- White hint bar at the bottom of the AUX views with the return hint on
//-- the left.
static void drawBottomHint(int panelWidth, int panelHeight)
{
  tft_ec11_fill_rect(0, panelHeight - PANEL_HINT_H, panelWidth, PANEL_HINT_H, TFT_EC11_WHITE);
  tft_ec11_draw_text_font(PANEL_EDGE_MARGIN, panelHeight - PANEL_HINT_H + (PANEL_HINT_H - fontCellHeight(FONT_STAMP)) / 2,
                          "AUX: Terug", FONT_STAMP, TFT_EC11_WHITE, TFT_EC11_BLACK);
}

//-- AUX power text screen: delivered and returned watts of the phase the
//-- encoder selected (0-2 Fase 1-3, 3 Totaal).
static void drawPowerTextView(void)
{
  const loggerPowerSnapshot *snapshot = &board.panel;
  int width = (int)tft_ec11_width();
  int height = (int)tft_ec11_height();
  uint8_t index = board.powerIndex;
  float delivered = (index < 3u) ? snapshot->phaseDeliveredWatts[index] : snapshot->totalDeliveredWatts;
  float returned = (index < 3u) ? snapshot->phaseReturnedWatts[index] : snapshot->totalReturnedWatts;
  char text[32];

  drawScreenTitle(width, "PER FASE", "", phaseLabels[index]);
  tft_ec11_fill_rect(0, PANEL_TITLE_H, width, height - PANEL_TITLE_H - PANEL_HINT_H, TFT_EC11_BLACK);

  drawLeftText(PANEL_TITLE_H + 16, "Geleverd vermogen", FONT_INFO, TFT_EC11_BLACK, TFT_EC11_WHITE);
  snprintf(text, sizeof text, "%.1f W", (double)delivered);
  drawCenteredText(PANEL_TITLE_H + 44, width, text, FONT_DATA, TFT_EC11_BLACK, PANEL_CONSUME_COLOR);

  drawLeftText(PANEL_TITLE_H + 86, "Opgewekte vermogen", FONT_INFO, TFT_EC11_BLACK, TFT_EC11_WHITE);
  snprintf(text, sizeof text, "%.1f W", (double)returned);
  drawCenteredText(PANEL_TITLE_H + 114, width, text, FONT_DATA, TFT_EC11_BLACK, PANEL_PRODUCE_COLOR);

  drawBottomHint(width, height);
}

//-- AUX medium-press view: reception counters, network and configuration.
static void drawSystemView(void)
{
  const loggerPowerSnapshot *snapshot = &board.panel;
  int width = (int)tft_ec11_width();
  int height = (int)tft_ec11_height();
  char line[64];
  int y = PANEL_TITLE_H + 10;
  const int pitch = 16;

  drawScreenTitle(width, "SYSTEMINFO", "", "");
  tft_ec11_fill_rect(0, PANEL_TITLE_H, width, height - PANEL_TITLE_H - PANEL_HINT_H, TFT_EC11_BLACK);

  snprintf(line, sizeof line, "Goedgekeurde telegrammen : %" PRIu32, snapshot->acceptedCount);
  drawLeftText(y, line, FONT_LABEL, TFT_EC11_BLACK, TFT_EC11_WHITE);
  y += pitch;

  snprintf(line, sizeof line, "Afgekeurde telegrammen : %" PRIu32, snapshot->rejectedCount);
  drawLeftText(y, line, FONT_LABEL, TFT_EC11_BLACK, TFT_EC11_WHITE);
  y += pitch;

  snprintf(line, sizeof line, "IP-adres : %s", (board.ip[0] != '\0') ? board.ip : "geen");
  drawLeftText(y, line, FONT_LABEL, TFT_EC11_BLACK, TFT_EC11_WHITE);
  y += pitch;

  snprintf(line, sizeof line, "SSID : %s", (board.ssid[0] != '\0') ? board.ssid : "geen");
  drawLeftText(y, line, FONT_LABEL, TFT_EC11_BLACK, TFT_EC11_WHITE);
  y += pitch;

  snprintf(line, sizeof line, "Uptime : %" PRIu32 "d %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
           snapshot->uptimeSeconds / 86400u, (snapshot->uptimeSeconds / 3600u) % 24u,
           (snapshot->uptimeSeconds / 60u) % 60u, snapshot->uptimeSeconds % 60u);
  drawLeftText(y, line, FONT_LABEL, TFT_EC11_BLACK, TFT_EC11_WHITE);
  y += pitch;

  snprintf(line, sizeof line, "Land/protocol : %s %s",
           (snapshot->countryCode[0] != '\0') ? snapshot->countryCode : "?",
           (snapshot->profileId[0] != '\0') ? snapshot->profileId : "?");
  drawLeftText(y, line, FONT_LABEL, TFT_EC11_BLACK, TFT_EC11_WHITE);
  y += pitch;

  snprintf(line, sizeof line, "P1 RX : %s", snapshot->rxInverted ? "inverted" : "normal");
  drawLeftText(y, line, FONT_LABEL, TFT_EC11_BLACK, TFT_EC11_WHITE);

  drawBottomHint(width, height);
}

static void drawActiveView(void)
{
  if (board.mode == BOARD_MODE_POWER_TEXT)
  {
    drawPowerTextView();
    return;
  }

  if (board.mode == BOARD_MODE_SYSTEM)
  {
    drawSystemView();
    return;
  }

  drawPanel();
}

static void drawActiveViewCleared(void)
{
  tft_ec11_clear();
  drawActiveView();
}

static void refreshAll(void)
{
  if (board.panelActive)
  {
    drawActiveView();
    return;
  }

  refreshDebugLines();
}

//-- Input task: short AUX press toggles between the graphics panel and the
//-- power text screen, a medium-long press swaps the bars for the system
//-- view (a short press returns), and the encoder cycles the phase shown on
//-- the text screen. Events only act once the panel has taken over.
static void handleBoardEvent(const tft_ec11_event_t *event)
{
  if (!board.panelActive)
  {
    return;
  }

  if (event->type == TFT_EC11_EVENT_AUX_BUTTON)
  {
    if (event->press != TFT_EC11_PRESS_SHORT)
    {
      if ((event->press == TFT_EC11_PRESS_MEDIUM) && (board.mode == BOARD_MODE_GRAPHICS))
      {
        xSemaphoreTake(board.mutex, portMAX_DELAY);
        board.mode = BOARD_MODE_SYSTEM;
        drawActiveViewCleared();
        xSemaphoreGive(board.mutex);
      }

      return;
    }

    xSemaphoreTake(board.mutex, portMAX_DELAY);

    if (board.mode == BOARD_MODE_GRAPHICS)
    {
      board.mode = BOARD_MODE_POWER_TEXT;
      board.powerIndex = PANEL_ROWS - 1u;
    }
    else
    {
      board.mode = BOARD_MODE_GRAPHICS;
    }

    drawActiveViewCleared();
    xSemaphoreGive(board.mutex);
    return;
  }

  if ((event->type == TFT_EC11_EVENT_ROTATE) && (board.mode == BOARD_MODE_POWER_TEXT) && (event->steps != 0))
  {
    int wrapped = (((int)board.powerIndex + (int)event->steps) % PANEL_ROWS + PANEL_ROWS) % PANEL_ROWS;

    xSemaphoreTake(board.mutex, portMAX_DELAY);
    board.powerIndex = (uint8_t)wrapped;
    drawActiveView();
    xSemaphoreGive(board.mutex);
  }
}

static void boardInputsTask(void *arg)
{
  tft_ec11_event_t event;
  (void)arg;

  for (;;)
  {
    if (tft_ec11_get_event(&event, pdMS_TO_TICKS(200)))
    {
      handleBoardEvent(&event);
    }
  }
}

static void update(char *field, size_t fieldSize, const char *value)
{
  if (!board.available)
  {
    return;
  }

  xSemaphoreTake(board.mutex, portMAX_DELAY);
  snprintf(field, fieldSize, "%s", (value != NULL) ? value : "");

  //-- While the power panel is active nothing else is drawn on the screen;
  //-- the strings stay stored for a future refresh.
  if (!board.panelActive)
  {
    refreshAll();
  }

  xSemaphoreGive(board.mutex);
}

esp_err_t loggerBoardInit(void)
{
  memset(&board, 0, sizeof board);
  board.mutex = xSemaphoreCreateMutex();

  if (board.mutex == NULL)
  {
    return ESP_ERR_NO_MEM;
  }

  esp_err_t result = tft_ec11_init();

  if (result != ESP_OK)
  {
    //-- No panel: the logger stays fully functional without debug lines.
    ESP_LOGW(tag, "display init failed: %s, continuing without debug lines", esp_err_to_name(result));
    return result;
  }

  tft_ec11_set_background(TFT_EC11_BLACK);
  tft_ec11_set_text_style(TFT_EC11_WHITE, BOARD_FONT_SCALE);
  tft_ec11_clear();

  board.available = true;
  board.mode = BOARD_MODE_GRAPHICS;
  snprintf(board.screen, sizeof board.screen, "boot");
  snprintf(board.p1Note, sizeof board.p1Note, "waiting");
  snprintf(board.wifiStatus, sizeof board.wifiStatus, "starting");
  refreshAll();

  //-- The AUX button and the encoder drive the panel views; when the inputs
  //-- cannot be started the panel keeps working without them.
  esp_err_t inputs = tft_ec11_inputs_start(BOARD_INPUT_QUEUE_LEN);

  if (inputs == ESP_OK)
  {
    if (xTaskCreate(boardInputsTask, "boardInputs", 3072, NULL, 4, NULL) != pdPASS)
    {
      tft_ec11_inputs_stop();
      ESP_LOGW(tag, "input task could not start, AUX button and encoder disabled");
    }
  }
  else
  {
    ESP_LOGW(tag, "board inputs not started: %s", esp_err_to_name(inputs));
  }

  ESP_LOGI(tag, "debug lines ready on the board display");
  return ESP_OK;
}

void loggerBoardSetRotation(bool upsideDown)
{
  if (!board.available)
  {
    return;
  }

  xSemaphoreTake(board.mutex, portMAX_DELAY);
  tft_ec11_set_rotation(upsideDown ? BOARD_ROTATION_UPSIDE_DOWN : BOARD_ROTATION_NORMAL);

  //-- The panel geometry follows the width and height the component reports,
  //-- so a full redraw lays the active view out for the new orientation.
  tft_ec11_clear();
  refreshAll();
  xSemaphoreGive(board.mutex);

  ESP_LOGI(tag, "display orientation: %s", upsideDown ? "upside down" : "normal");
}

void loggerBoardShowScreen(const char *screen)
{
  update(board.screen, sizeof board.screen, screen);
}

void loggerBoardShowParserVersion(const char *version)
{
  update(board.parserVersion, sizeof board.parserVersion, version);
}

void loggerBoardNoteTelegram(uint32_t sequence)
{
  char note[BOARD_TEXT_MAX];
  snprintf(note, sizeof note, "telegram %lu accepted", (unsigned long)sequence);
  update(board.p1Note, sizeof board.p1Note, note);
}

void loggerBoardWifiPortal(const char *url)
{
  update(board.wifiStatus, sizeof board.wifiStatus, "portal open");

  char ipLine[BOARD_TEXT_MAX];
  snprintf(ipLine, sizeof ipLine, "%s", (url != NULL) ? url : "http://192.168.4.1/");
  update(board.wifiIp, sizeof board.wifiIp, ipLine);
}

void loggerBoardWifiConnected(const char *ssid, const char *ip)
{
  char status[BOARD_TEXT_MAX];
  snprintf(status, sizeof status, "joined %s", (ssid != NULL) ? ssid : "?");
  update(board.wifiStatus, sizeof board.wifiStatus, status);

  char ipLine[BOARD_TEXT_MAX];
  snprintf(ipLine, sizeof ipLine, "http://%s", (ip != NULL) ? ip : "?");
  update(board.wifiIp, sizeof board.wifiIp, ipLine);

  //-- Plain SSID and IP for the system information view.
  xSemaphoreTake(board.mutex, portMAX_DELAY);
  snprintf(board.ssid, sizeof board.ssid, "%s", (ssid != NULL) ? ssid : "");
  snprintf(board.ip, sizeof board.ip, "%s", (ip != NULL) ? ip : "");
  xSemaphoreGive(board.mutex);
}

void loggerBoardShowPower(const loggerPowerSnapshot *snapshot)
{
  if (!board.available || (snapshot == NULL))
  {
    return;
  }

  xSemaphoreTake(board.mutex, portMAX_DELAY);
  board.panel = *snapshot;

  //-- The first power frame takes over the whole screen: the boot debug
  //-- lines are wiped before the panel is drawn.
  if (!board.panelActive)
  {
    tft_ec11_clear();
  }

  board.panelActive = true;

  //-- A new telegram only refreshes the graphics panel; the AUX views keep
  //-- the stored frame and wait for their own short press to return.
  if (board.mode == BOARD_MODE_GRAPHICS)
  {
    drawPanel();
  }

  xSemaphoreGive(board.mutex);
}
