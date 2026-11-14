//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_board.c - on-device TFT debug lines
//-- Date: 2026-11-14 · Build: 1
//--
//-- Debug lines are drawn through the documented API of the esp32_s3_piggyback
//-- component: tft_ec11_init once, then fixed-width text slots. The encoder
//-- and button inputs are never started, so their GPIOs stay free. Drawing is
//-- serialized with a mutex because WiFi callbacks, the receive task and the
//-- HTTP handlers all report state.
//-- ==========================================================================

#include "logger_board.h"

#include <stdio.h>
#include <string.h>

#include "esp32_s3_piggyback.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *tag = "loggerBoard";

//-- Scale 2 gives a 12 by 16 pixel cell: 25 characters fit on the 320 pixel
//-- wide panel with a small left margin.
#define BOARD_FONT_SCALE 2
#define BOARD_CHARS_PER_LINE 25
#define BOARD_LEFT_X 4
#define BOARD_LINE_PITCH (8 * BOARD_FONT_SCALE)

#define BOARD_LINE_SCREEN 2
#define BOARD_LINE_P1 3
#define BOARD_LINE_WIFI 4
#define BOARD_LINE_IP 5

#define BOARD_TEXT_MAX 48

typedef struct
{
  bool available;
  SemaphoreHandle_t mutex;
  char screen[BOARD_TEXT_MAX];
  char p1Note[BOARD_TEXT_MAX];
  char wifiStatus[BOARD_TEXT_MAX];
  char wifiIp[BOARD_TEXT_MAX];
} LoggerBoard;

static LoggerBoard board;

static void drawLine(uint8_t row, const char *label, const char *value)
{
  char line[BOARD_CHARS_PER_LINE + 1];
  snprintf(line, sizeof line, "%s%s", label, value);

  tft_ec11_draw_text(BOARD_LEFT_X, (int)(row * BOARD_LINE_PITCH), BOARD_CHARS_PER_LINE, line);
}

static void refreshAll(void)
{
  drawLine(BOARD_LINE_SCREEN, "screen : ", board.screen);
  drawLine(BOARD_LINE_P1, "p1     : ", board.p1Note);
  drawLine(BOARD_LINE_WIFI, "wifi   : ", board.wifiStatus);
  drawLine(BOARD_LINE_IP, "ip     : ", board.wifiIp);
}

static void update(char *field, size_t fieldSize, const char *value)
{
  if (!board.available)
  {
    return;
  }

  xSemaphoreTake(board.mutex, portMAX_DELAY);
  snprintf(field, fieldSize, "%s", (value != NULL) ? value : "");
  refreshAll();
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
  snprintf(board.screen, sizeof board.screen, "boot");
  snprintf(board.p1Note, sizeof board.p1Note, "waiting");
  snprintf(board.wifiStatus, sizeof board.wifiStatus, "starting");
  refreshAll();

  ESP_LOGI(tag, "debug lines ready on the board display");
  return ESP_OK;
}

void loggerBoardShowScreen(const char *screen)
{
  update(board.screen, sizeof board.screen, screen);
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
}
