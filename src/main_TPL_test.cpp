/**
 * TPL5110 / Reed Hardware-Test (Gas-O-Meter2)
 *
 * Minimalfirmware: kein LP-Core, kein WiFi, kein Web.
 * GPIO2 (DRV vom TPL5110) per ANYEDGE-Interrupt + Zeitstempel (esp_timer).
 *
 * Build / Upload / Monitor:
 *   pio run -e TPL_test -t upload
 *   pio device monitor -e TPL_test
 *
 * Erwartung bei dauerhaft geschlossenem Reed (Drahtbrücke):
 *   lange LOW (~3–4 s), kurzes HIGH (~50 ms), wieder LOW …
 * Bei kurzem Reed-Impuls: ein LOW-Fenster ~3–4 s, danach stabil HIGH.
 *
 * Pin: REED/DRV = GPIO2 (XIAO ESP32-C6 D2), siehe include/hardware.h
 *
 * Hinweis XIAO ESP32-C6: USB-CDC braucht nach Reset ~1–2 s, sonst gehen
 * frühe Logs verloren (wie in main_idf.cpp).
 */

#include <stdio.h>
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "TPL_test";

/** Muss zu hardware.h REED_GPIO passen (TPL5110 DRV → GPIO2). */
#define REED_GPIO GPIO_NUM_2

/** Pause nach Boot, damit der USB-Host den CDC-Port öffnen kann. */
#define USB_CDC_SETTLE_MS 2000

/** Lebenszeichen, falls keine Flanken kommen. */
#define HEARTBEAT_MS 2000

typedef struct {
    int64_t t_us;
    int level;
} edge_evt_t;

static QueueHandle_t s_edge_q;

static void IRAM_ATTR reed_gpio_isr(void *arg)
{
    (void)arg;
    edge_evt_t evt = {
        .t_us = esp_timer_get_time(),
        .level = gpio_get_level(REED_GPIO),
    };
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_edge_q, &evt, &hpw);
    if (hpw) {
        portYIELD_FROM_ISR(hpw);
    }
}

static void print_banner(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Gas-O-Meter2 — TPL5110 Hardware-Test");
    ESP_LOGI(TAG, "  GPIO%d ANYEDGE + esp_timer (µs)", (int)REED_GPIO);
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Reed kurz → DRV-Pulse; Monitor zeigt Flanken + Dauer.");
    ESP_LOGI(TAG, "Format: t_us  dt_us  LEVEL  (dt = Zeit seit letzter Flanke)");
    fflush(stdout);
}

extern "C" void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    /* Wie Produktivfirmware: USB-CDC am XIAO C6 erst nach Enumeration nutzbar */
    vTaskDelay(pdMS_TO_TICKS(USB_CDC_SETTLE_MS));

    print_banner();

    s_edge_q = xQueueCreate(64, sizeof(edge_evt_t));
    if (!s_edge_q) {
        ESP_LOGE(TAG, "Queue-Erzeugung fehlgeschlagen");
        return;
    }

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << REED_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&io));

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(REED_GPIO, reed_gpio_isr, nullptr));

    const int level0 = gpio_get_level(REED_GPIO);
    const int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "%" PRId64 "  (start)  %s", t0, level0 ? "HIGH" : "LOW");
    fflush(stdout);

    int64_t t_prev = t0;
    int level_prev = level0;

    while (true) {
        edge_evt_t evt;
        if (xQueueReceive(s_edge_q, &evt, pdMS_TO_TICKS(HEARTBEAT_MS)) != pdTRUE) {
            ESP_LOGI(TAG, "heartbeat  GPIO%d=%s  t=%" PRId64,
                     (int)REED_GPIO,
                     gpio_get_level(REED_GPIO) ? "HIGH" : "LOW",
                     esp_timer_get_time());
            fflush(stdout);
            continue;
        }

        /* Entprellen gleicher Pegel (Doppel-ISR / Glitch) */
        if (evt.level == level_prev) {
            continue;
        }

        const int64_t dt = evt.t_us - t_prev;
        ESP_LOGI(TAG, "%" PRId64 "  %+" PRId64 " us  %s",
                 evt.t_us, dt, evt.level ? "HIGH" : "LOW");

        /* Hinweis für typische TPL-Lücke (~50 ms HIGH bei Dauer-Trigger) */
        if (evt.level == 0 && dt > 20000 && dt < 150000) {
            ESP_LOGW(TAG, "  ^ kurzes HIGH ~%" PRId64 " ms (TPL-Retrigger-Lücke?)",
                     dt / 1000);
        }
        fflush(stdout);

        t_prev = evt.t_us;
        level_prev = evt.level;
    }
}
