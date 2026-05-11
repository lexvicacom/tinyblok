#include "display.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "display";

#define LCD_ADDR_PRIMARY 0x27
#define LCD_ADDR_SECONDARY 0x3F
#define LCD_SDA_GPIO GPIO_NUM_14
#define LCD_SCL_GPIO GPIO_NUM_15
#define LCD_I2C_HZ 10000
#define LCD_COLS 16

#ifndef TINYBLOK_VERSION
#define TINYBLOK_VERSION "dev"
#endif

#define LCD_RS BIT0
#define LCD_RW BIT1
#define LCD_EN BIT2
#define LCD_BACKLIGHT BIT3

#define DISPLAY_NOTIFY_REDRAW BIT0

typedef struct
{
    bool wifi_connected;
    char ssid[33];
    char ip[16];
    char nats_host[64];
    char nats_ip[16];
    uint16_t nats_port;
} display_state_t;

static TaskHandle_t display_task_handle;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t lcd_dev;
static bool display_available;
static uint8_t lcd_addr;
static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
static display_state_t state;

static void display_notify(void)
{
    TaskHandle_t task = display_task_handle;
    if (display_available && task != NULL)
        xTaskNotify(task, DISPLAY_NOTIFY_REDRAW, eSetBits);
}

void tinyblok_display_wifi_connecting(const char *ssid)
{
    portENTER_CRITICAL(&state_mux);
    state.wifi_connected = false;
    strlcpy(state.ssid, ssid != NULL ? ssid : "", sizeof(state.ssid));
    state.ip[0] = '\0';
    portEXIT_CRITICAL(&state_mux);
    display_notify();
}

void tinyblok_display_wifi_connected(const char *ssid, const esp_ip4_addr_t *ip)
{
    portENTER_CRITICAL(&state_mux);
    state.wifi_connected = true;
    strlcpy(state.ssid, ssid != NULL ? ssid : "", sizeof(state.ssid));
    if (ip != NULL)
        snprintf(state.ip, sizeof(state.ip), IPSTR, IP2STR(ip));
    else
        state.ip[0] = '\0';
    portEXIT_CRITICAL(&state_mux);
    display_notify();
}

void tinyblok_display_wifi_disconnected(void)
{
    portENTER_CRITICAL(&state_mux);
    state.wifi_connected = false;
    state.ip[0] = '\0';
    portEXIT_CRITICAL(&state_mux);
    display_notify();
}

static void app_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == TINYBLOK_EVENT_MESSAGE_PROCESSED || id == TINYBLOK_EVENT_PUB_SENT)
    {
        display_notify();
        return;
    }

    if (id == TINYBLOK_EVENT_NATS_CONNECTED)
    {
        const tinyblok_nats_connected_event_t *event = (const tinyblok_nats_connected_event_t *)data;
        if (event == NULL)
            return;

        portENTER_CRITICAL(&state_mux);
        strlcpy(state.nats_host, event->host, sizeof(state.nats_host));
        strlcpy(state.nats_ip, event->ip, sizeof(state.nats_ip));
        state.nats_port = event->port;
        portEXIT_CRITICAL(&state_mux);
        display_notify();
    }
    else if (id == TINYBLOK_EVENT_NATS_DISCONNECTED)
    {
        portENTER_CRITICAL(&state_mux);
        state.nats_host[0] = '\0';
        state.nats_ip[0] = '\0';
        state.nats_port = 0;
        portEXIT_CRITICAL(&state_mux);
        display_notify();
    }
}

static void disable_display(const char *what, esp_err_t err)
{
    if (display_available)
        ESP_LOGW(TAG, "%s failed: %s; disabling LCD", what, esp_err_to_name(err));
    display_available = false;
}

static esp_err_t lcd_tx_byte(uint8_t byte)
{
    if (!display_available)
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2c_master_transmit(lcd_dev, &byte, 1, 100);
    if (err != ESP_OK)
        disable_display("lcd i2c transmit", err);
    return err;
}

static esp_err_t lcd_write4(uint8_t nibble, uint8_t flags)
{
    uint8_t out = (uint8_t)((nibble & 0x0F) << 4) | LCD_BACKLIGHT | flags;
    ESP_RETURN_ON_ERROR(lcd_tx_byte((uint8_t)(out | LCD_EN)), TAG, "lcd enable high");
    esp_rom_delay_us(1);
    ESP_RETURN_ON_ERROR(lcd_tx_byte((uint8_t)(out & ~LCD_EN)), TAG, "lcd enable low");
    esp_rom_delay_us(50);
    return ESP_OK;
}

static esp_err_t lcd_write8(uint8_t value, uint8_t flags)
{
    ESP_RETURN_ON_ERROR(lcd_write4((uint8_t)(value >> 4), flags), TAG, "lcd high nibble");
    return lcd_write4((uint8_t)(value & 0x0F), flags);
}

static esp_err_t lcd_cmd(uint8_t cmd)
{
    ESP_RETURN_ON_ERROR(lcd_write8(cmd, 0), TAG, "lcd command");
    if (cmd == 0x01 || cmd == 0x02)
        vTaskDelay(pdMS_TO_TICKS(2));
    return ESP_OK;
}

static esp_err_t lcd_data(uint8_t data)
{
    return lcd_write8(data, LCD_RS);
}

static esp_err_t lcd_clear(void)
{
    return lcd_cmd(0x01);
}

static esp_err_t lcd_set_cursor(uint8_t row, uint8_t col)
{
    static const uint8_t row_addr[2] = {0x00, 0x40};
    if (row > 1)
        row = 1;
    if (col >= LCD_COLS)
        col = LCD_COLS - 1;
    return lcd_cmd((uint8_t)(0x80 | row_addr[row] | col));
}

static esp_err_t lcd_write_padded(const char *text)
{
    size_t i = 0;
    for (; i < LCD_COLS && text[i] != '\0'; i++)
        ESP_RETURN_ON_ERROR(lcd_data((uint8_t)text[i]), TAG, "lcd write char");
    for (; i < LCD_COLS; i++)
        ESP_RETURN_ON_ERROR(lcd_data(' '), TAG, "lcd pad char");
    return ESP_OK;
}

static esp_err_t create_i2c_bus(gpio_num_t sda_gpio, gpio_num_t scl_gpio)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &i2c_bus);
}

static unsigned scan_i2c_bus(gpio_num_t sda_gpio, gpio_num_t scl_gpio)
{
    unsigned found_count = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++)
    {
        esp_err_t err = i2c_master_probe(i2c_bus, addr, 20);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "I2C device found at 0x%02X on SDA GPIO%d SCL GPIO%d",
                     addr, sda_gpio, scl_gpio);
            found_count++;
        }
    }
    if (found_count == 0)
        ESP_LOGW(TAG, "I2C scan found no devices on SDA GPIO%d SCL GPIO%d",
                 sda_gpio, scl_gpio);
    return found_count;
}

static esp_err_t lcd_probe_address(uint8_t addr)
{
    esp_err_t err = i2c_master_probe(i2c_bus, addr, 100);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "HD44780 I2C backpack responded at 0x%02X", addr);
    else
        ESP_LOGI(TAG, "no HD44780 I2C backpack response at 0x%02X: %s",
                 addr, esp_err_to_name(err));
    return err;
}

static esp_err_t lcd_init_hw(void)
{
    const gpio_config_t preflight = {
        .pin_bit_mask = (1ULL << LCD_SDA_GPIO) | (1ULL << LCD_SCL_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&preflight), TAG, "configure i2c preflight pins");
    vTaskDelay(pdMS_TO_TICKS(10));

    const int sda_level = gpio_get_level(LCD_SDA_GPIO);
    const int scl_level = gpio_get_level(LCD_SCL_GPIO);
    if (sda_level == 0 || scl_level == 0)
    {
        ESP_LOGW(TAG, "I2C bus held low before LCD probe: SDA=%d SCL=%d; disabling display",
                 sda_level, scl_level);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(create_i2c_bus(LCD_SDA_GPIO, LCD_SCL_GPIO), TAG, "create i2c bus");
    unsigned found_count = scan_i2c_bus(LCD_SDA_GPIO, LCD_SCL_GPIO);
    if (found_count == 0)
    {
        i2c_del_master_bus(i2c_bus);
        i2c_bus = NULL;
        ESP_RETURN_ON_ERROR(create_i2c_bus(LCD_SCL_GPIO, LCD_SDA_GPIO), TAG, "create swapped i2c bus");
        found_count = scan_i2c_bus(LCD_SCL_GPIO, LCD_SDA_GPIO);
        i2c_del_master_bus(i2c_bus);
        i2c_bus = NULL;
        if (found_count > 0)
            ESP_LOGW(TAG, "I2C device only found with swapped pins; check SDA/SCL wiring");
        ESP_RETURN_ON_ERROR(create_i2c_bus(LCD_SDA_GPIO, LCD_SCL_GPIO), TAG, "restore i2c bus");
    }

    ESP_LOGI(TAG, "probing HD44780 I2C backpack addresses 0x%02X and 0x%02X on SDA GPIO%d SCL GPIO%d",
             LCD_ADDR_PRIMARY, LCD_ADDR_SECONDARY, LCD_SDA_GPIO, LCD_SCL_GPIO);
    esp_err_t err = lcd_probe_address(LCD_ADDR_PRIMARY);
    if (err == ESP_OK)
    {
        lcd_addr = LCD_ADDR_PRIMARY;
    }
    else
    {
        err = lcd_probe_address(LCD_ADDR_SECONDARY);
        if (err != ESP_OK)
            return err;
        lcd_addr = LCD_ADDR_SECONDARY;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = lcd_addr,
        .scl_speed_hz = LCD_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &lcd_dev), TAG, "add lcd device");

    display_available = true;
    vTaskDelay(pdMS_TO_TICKS(50));

    // HD44780 4-bit initialization through the PCF8574 expander.
    ESP_RETURN_ON_ERROR(lcd_write4(0x03, 0), TAG, "lcd init 8-bit 1");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(lcd_write4(0x03, 0), TAG, "lcd init 8-bit 2");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(lcd_write4(0x03, 0), TAG, "lcd init 8-bit 3");
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_RETURN_ON_ERROR(lcd_write4(0x02, 0), TAG, "lcd init 4-bit");

    ESP_RETURN_ON_ERROR(lcd_cmd(0x28), TAG, "lcd function set");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x08), TAG, "lcd display off");
    ESP_RETURN_ON_ERROR(lcd_clear(), TAG, "lcd clear");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x06), TAG, "lcd entry mode");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x0C), TAG, "lcd display on");
    return ESP_OK;
}

static void draw_connecting(uint32_t tick)
{
    char line1[LCD_COLS + 1];
    char line2[LCD_COLS + 1];
    snprintf(line1, sizeof(line1), "Hello!");
    snprintf(line2, sizeof(line2), "WiFi%.*s", (int)(tick % 4), "...");
    lcd_set_cursor(0, 0);
    lcd_write_padded(line1);
    lcd_set_cursor(1, 0);
    lcd_write_padded(line2);
}

static void draw_status(void)
{
    display_state_t copy;
    portENTER_CRITICAL(&state_mux);
    copy = state;
    portEXIT_CRITICAL(&state_mux);

    char line1[LCD_COLS + 1];
    char line2[LCD_COLS + 1];
    snprintf(line1, sizeof(line1), "tinyblok NATS %s",
             copy.nats_ip[0] != '\0' ? "OK" : "--");
    snprintf(line2, sizeof(line2), "PUBs: %lu", (unsigned long)tinyblok_pub_count());

    lcd_set_cursor(0, 0);
    lcd_write_padded(line1);
    lcd_set_cursor(1, 0);
    lcd_write_padded(line2);
}

static void display_task(void *arg)
{
    (void)arg;
    display_task_handle = xTaskGetCurrentTaskHandle();

    esp_err_t err = lcd_init_hw();
    if (err != ESP_OK)
    {
        display_available = false;
        ESP_LOGW(TAG, "no HD44780 I2C backpack detected at 0x%02X/0x%02X: %s; continuing without display",
                 LCD_ADDR_PRIMARY, LCD_ADDR_SECONDARY, esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "HD44780 16x2 LCD ready at 0x%02X", lcd_addr);
    esp_err_t reg_err = esp_event_handler_register(TINYBLOK_EVENT, ESP_EVENT_ANY_ID,
                                                   &app_event_handler, NULL);
    if (reg_err != ESP_OK)
        ESP_LOGW(TAG, "display event registration failed: %s", esp_err_to_name(reg_err));

    lcd_clear();
    lcd_set_cursor(0, 0);
    char splash[LCD_COLS + 1];
    snprintf(splash, sizeof(splash), "tinyblok v%.6s", TINYBLOK_VERSION);
    lcd_write_padded(splash);
    vTaskDelay(pdMS_TO_TICKS(1000));

    uint32_t anim_tick = 0;
    while (true)
    {
        if (!display_available)
            vTaskDelete(NULL);

        display_state_t copy;
        portENTER_CRITICAL(&state_mux);
        copy = state;
        portEXIT_CRITICAL(&state_mux);

        if (copy.wifi_connected)
            draw_status();
        else
            draw_connecting(anim_tick++);

        uint32_t notify = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notify, pdMS_TO_TICKS(copy.wifi_connected ? 1000 : 300));
    }
}

void tinyblok_display_start(void)
{
    xTaskCreate(display_task, "display", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
}
