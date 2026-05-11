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

#define OLED_ADDR_PRIMARY 0x3C
#define OLED_ADDR_SECONDARY 0x3D
#define OLED_I2C_HZ 400000
#define OLED_WIDTH 128

#if CONFIG_TINYBLOK_DISPLAY_OLED_128X32
#define OLED_HEIGHT 32
#define OLED_COM_PINS 0x02
#define OLED_TEXT_FIRST_PAGE 0
#if CONFIG_TINYBLOK_DISPLAY_OLED_128X32_THREE_ROWS
#define OLED_TEXT_PAGE_STEP 1
#define OLED_TEXT_ROWS 3
#else
#define OLED_TEXT_PAGE_STEP 2
#define OLED_TEXT_ROWS 2
#endif
#elif CONFIG_TINYBLOK_DISPLAY_OLED_128X48
#define OLED_HEIGHT 48
#define OLED_COM_PINS 0x12
#define OLED_TEXT_FIRST_PAGE 0
#define OLED_TEXT_PAGE_STEP 2
#define OLED_TEXT_ROWS 3
#else
#define OLED_HEIGHT 64
#define OLED_COM_PINS 0x12
#define OLED_TEXT_FIRST_PAGE 1
#define OLED_TEXT_PAGE_STEP 2
#define OLED_TEXT_ROWS 3
#endif

#define OLED_PAGE_COUNT (OLED_HEIGHT / 8)
#define OLED_FONT_WIDTH 5
#define OLED_CELL_WIDTH 6

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

typedef struct
{
    const char *name;
    uint8_t rows;
    esp_err_t (*clear)(void);
    esp_err_t (*set_cursor)(uint8_t row, uint8_t col);
    esp_err_t (*write_padded)(const char *text);
} display_driver_t;

static TaskHandle_t display_task_handle;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t lcd_dev;
static i2c_master_dev_handle_t oled_dev;
static bool display_available;
static uint8_t lcd_addr;
static uint8_t oled_addr;
static const display_driver_t *active_display;
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

static const display_driver_t lcd_driver = {
    .name = "HD44780 16x2 LCD",
    .rows = 2,
    .clear = lcd_clear,
    .set_cursor = lcd_set_cursor,
    .write_padded = lcd_write_padded,
};

static esp_err_t oled_tx(const uint8_t *bytes, size_t len)
{
    if (!display_available)
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2c_master_transmit(oled_dev, bytes, len, 100);
    if (err != ESP_OK)
        disable_display("oled i2c transmit", err);
    return err;
}

static esp_err_t oled_cmd(uint8_t cmd)
{
    const uint8_t bytes[2] = {0x00, cmd};
    return oled_tx(bytes, sizeof(bytes));
}

static esp_err_t oled_cmd2(uint8_t cmd, uint8_t arg)
{
    const uint8_t bytes[3] = {0x00, cmd, arg};
    return oled_tx(bytes, sizeof(bytes));
}

static const uint8_t *oled_glyph(char ch)
{
    static const uint8_t blank[OLED_FONT_WIDTH] = {0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t bang[OLED_FONT_WIDTH] = {0x00, 0x00, 0x5F, 0x00, 0x00};
    static const uint8_t dash[OLED_FONT_WIDTH] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t dot[OLED_FONT_WIDTH] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t colon[OLED_FONT_WIDTH] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t digit0[OLED_FONT_WIDTH] = {0x3E, 0x51, 0x49, 0x45, 0x3E};
    static const uint8_t digit1[OLED_FONT_WIDTH] = {0x00, 0x42, 0x7F, 0x40, 0x00};
    static const uint8_t digit2[OLED_FONT_WIDTH] = {0x42, 0x61, 0x51, 0x49, 0x46};
    static const uint8_t digit3[OLED_FONT_WIDTH] = {0x21, 0x41, 0x45, 0x4B, 0x31};
    static const uint8_t digit4[OLED_FONT_WIDTH] = {0x18, 0x14, 0x12, 0x7F, 0x10};
    static const uint8_t digit5[OLED_FONT_WIDTH] = {0x27, 0x45, 0x45, 0x45, 0x39};
    static const uint8_t digit6[OLED_FONT_WIDTH] = {0x3C, 0x4A, 0x49, 0x49, 0x30};
    static const uint8_t digit7[OLED_FONT_WIDTH] = {0x01, 0x71, 0x09, 0x05, 0x03};
    static const uint8_t digit8[OLED_FONT_WIDTH] = {0x36, 0x49, 0x49, 0x49, 0x36};
    static const uint8_t digit9[OLED_FONT_WIDTH] = {0x06, 0x49, 0x49, 0x29, 0x1E};
    static const uint8_t glyph_a[OLED_FONT_WIDTH] = {0x20, 0x54, 0x54, 0x54, 0x78};
    static const uint8_t glyph_b[OLED_FONT_WIDTH] = {0x7F, 0x48, 0x44, 0x44, 0x38};
    static const uint8_t glyph_c[OLED_FONT_WIDTH] = {0x38, 0x44, 0x44, 0x44, 0x20};
    static const uint8_t glyph_d[OLED_FONT_WIDTH] = {0x38, 0x44, 0x44, 0x48, 0x7F};
    static const uint8_t glyph_e[OLED_FONT_WIDTH] = {0x38, 0x54, 0x54, 0x54, 0x18};
    static const uint8_t glyph_f[OLED_FONT_WIDTH] = {0x08, 0x7E, 0x09, 0x01, 0x02};
    static const uint8_t glyph_h[OLED_FONT_WIDTH] = {0x7F, 0x08, 0x04, 0x04, 0x78};
    static const uint8_t glyph_i[OLED_FONT_WIDTH] = {0x00, 0x44, 0x7D, 0x40, 0x00};
    static const uint8_t glyph_k[OLED_FONT_WIDTH] = {0x7F, 0x10, 0x28, 0x44, 0x00};
    static const uint8_t glyph_l[OLED_FONT_WIDTH] = {0x00, 0x41, 0x7F, 0x40, 0x00};
    static const uint8_t glyph_n[OLED_FONT_WIDTH] = {0x7C, 0x08, 0x04, 0x04, 0x78};
    static const uint8_t glyph_o[OLED_FONT_WIDTH] = {0x38, 0x44, 0x44, 0x44, 0x38};
    static const uint8_t glyph_s[OLED_FONT_WIDTH] = {0x48, 0x54, 0x54, 0x54, 0x20};
    static const uint8_t glyph_t[OLED_FONT_WIDTH] = {0x04, 0x3F, 0x44, 0x40, 0x20};
    static const uint8_t glyph_v[OLED_FONT_WIDTH] = {0x1C, 0x20, 0x40, 0x20, 0x1C};
    static const uint8_t glyph_y[OLED_FONT_WIDTH] = {0x0C, 0x50, 0x50, 0x50, 0x3C};
    static const uint8_t glyph_A[OLED_FONT_WIDTH] = {0x7E, 0x11, 0x11, 0x11, 0x7E};
    static const uint8_t glyph_B[OLED_FONT_WIDTH] = {0x7F, 0x49, 0x49, 0x49, 0x36};
    static const uint8_t glyph_F[OLED_FONT_WIDTH] = {0x7F, 0x09, 0x09, 0x09, 0x01};
    static const uint8_t glyph_H[OLED_FONT_WIDTH] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
    static const uint8_t glyph_K[OLED_FONT_WIDTH] = {0x7F, 0x08, 0x14, 0x22, 0x41};
    static const uint8_t glyph_N[OLED_FONT_WIDTH] = {0x7F, 0x02, 0x0C, 0x10, 0x7F};
    static const uint8_t glyph_O[OLED_FONT_WIDTH] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
    static const uint8_t glyph_P[OLED_FONT_WIDTH] = {0x7F, 0x09, 0x09, 0x09, 0x06};
    static const uint8_t glyph_S[OLED_FONT_WIDTH] = {0x46, 0x49, 0x49, 0x49, 0x31};
    static const uint8_t glyph_T[OLED_FONT_WIDTH] = {0x01, 0x01, 0x7F, 0x01, 0x01};
    static const uint8_t glyph_U[OLED_FONT_WIDTH] = {0x3F, 0x40, 0x40, 0x40, 0x3F};
    static const uint8_t glyph_W[OLED_FONT_WIDTH] = {0x7F, 0x20, 0x18, 0x20, 0x7F};

    if (ch >= '0' && ch <= '9')
    {
        static const uint8_t *const digits[10] = {
            digit0, digit1, digit2, digit3, digit4, digit5, digit6, digit7, digit8, digit9,
        };
        return digits[ch - '0'];
    }

    switch (ch)
    {
    case ' ':
        return blank;
    case '!':
        return bang;
    case '-':
        return dash;
    case '.':
        return dot;
    case ':':
        return colon;
    case 'A':
        return glyph_A;
    case 'B':
        return glyph_B;
    case 'F':
        return glyph_F;
    case 'H':
        return glyph_H;
    case 'K':
        return glyph_K;
    case 'N':
        return glyph_N;
    case 'O':
        return glyph_O;
    case 'P':
        return glyph_P;
    case 'S':
        return glyph_S;
    case 'T':
        return glyph_T;
    case 'U':
        return glyph_U;
    case 'W':
        return glyph_W;
    case 'a':
        return glyph_a;
    case 'b':
        return glyph_b;
    case 'c':
        return glyph_c;
    case 'd':
        return glyph_d;
    case 'e':
        return glyph_e;
    case 'f':
        return glyph_f;
    case 'h':
        return glyph_h;
    case 'i':
        return glyph_i;
    case 'k':
        return glyph_k;
    case 'l':
        return glyph_l;
    case 'n':
        return glyph_n;
    case 'o':
        return glyph_o;
    case 's':
        return glyph_s;
    case 't':
        return glyph_t;
    case 'v':
        return glyph_v;
    case 'y':
        return glyph_y;
    default:
        return blank;
    }
}

static esp_err_t oled_clear(void)
{
    uint8_t line[1 + OLED_WIDTH];
    memset(line, 0, sizeof(line));
    line[0] = 0x40;

    for (uint8_t page = 0; page < OLED_PAGE_COUNT; page++)
    {
        ESP_RETURN_ON_ERROR(oled_cmd((uint8_t)(0xB0 | page)), TAG, "oled page");
        ESP_RETURN_ON_ERROR(oled_cmd(0x00), TAG, "oled low column");
        ESP_RETURN_ON_ERROR(oled_cmd(0x10), TAG, "oled high column");
        ESP_RETURN_ON_ERROR(oled_tx(line, sizeof(line)), TAG, "oled clear page");
    }
    return ESP_OK;
}

static esp_err_t oled_set_cursor(uint8_t row, uint8_t col)
{
    const uint8_t page = OLED_TEXT_FIRST_PAGE + (uint8_t)(row * OLED_TEXT_PAGE_STEP);
    const uint8_t pixel_col = (uint8_t)(col * OLED_CELL_WIDTH);
    ESP_RETURN_ON_ERROR(oled_cmd((uint8_t)(0xB0 | page)), TAG, "oled page");
    ESP_RETURN_ON_ERROR(oled_cmd((uint8_t)(pixel_col & 0x0F)), TAG, "oled low column");
    return oled_cmd((uint8_t)(0x10 | (pixel_col >> 4)));
}

static esp_err_t oled_write_padded(const char *text)
{
    uint8_t bytes[1 + LCD_COLS * OLED_CELL_WIDTH];
    bytes[0] = 0x40;
    size_t out = 1;
    bool padding = false;

    for (size_t i = 0; i < LCD_COLS; i++)
    {
        if (!padding && text[i] == '\0')
            padding = true;
        const char ch = padding ? ' ' : text[i];
        const uint8_t *glyph = oled_glyph(ch);
        for (size_t j = 0; j < OLED_FONT_WIDTH; j++)
            bytes[out++] = glyph[j];
        bytes[out++] = 0x00;
    }

    return oled_tx(bytes, out);
}

static const display_driver_t oled_driver = {
    .name = "SSD1306 OLED",
    .rows = OLED_TEXT_ROWS,
    .clear = oled_clear,
    .set_cursor = oled_set_cursor,
    .write_padded = oled_write_padded,
};

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

static esp_err_t oled_probe_address(uint8_t addr)
{
    esp_err_t err = i2c_master_probe(i2c_bus, addr, 100);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "SSD1306 OLED responded at 0x%02X", addr);
    else
        ESP_LOGI(TAG, "no SSD1306 OLED response at 0x%02X: %s",
                 addr, esp_err_to_name(err));
    return err;
}

static esp_err_t lcd_init_driver(void)
{
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

    active_display = &lcd_driver;
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

static esp_err_t oled_init_driver(void)
{
    ESP_LOGI(TAG, "probing SSD1306 OLED addresses 0x%02X and 0x%02X on SDA GPIO%d SCL GPIO%d",
             OLED_ADDR_PRIMARY, OLED_ADDR_SECONDARY, LCD_SDA_GPIO, LCD_SCL_GPIO);
    esp_err_t err = oled_probe_address(OLED_ADDR_PRIMARY);
    if (err == ESP_OK)
    {
        oled_addr = OLED_ADDR_PRIMARY;
    }
    else
    {
        err = oled_probe_address(OLED_ADDR_SECONDARY);
        if (err != ESP_OK)
            return err;
        oled_addr = OLED_ADDR_SECONDARY;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = oled_addr,
        .scl_speed_hz = OLED_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &oled_dev), TAG, "add oled device");

    active_display = &oled_driver;
    display_available = true;
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(oled_cmd(0xAE), TAG, "oled display off");
    ESP_RETURN_ON_ERROR(oled_cmd2(0xD5, 0x80), TAG, "oled clock");
    ESP_RETURN_ON_ERROR(oled_cmd2(0xA8, OLED_HEIGHT - 1), TAG, "oled multiplex");
    ESP_RETURN_ON_ERROR(oled_cmd2(0xD3, 0x00), TAG, "oled offset");
    ESP_RETURN_ON_ERROR(oled_cmd(0x40), TAG, "oled start line");
    ESP_RETURN_ON_ERROR(oled_cmd2(0x8D, 0x14), TAG, "oled charge pump");
    ESP_RETURN_ON_ERROR(oled_cmd2(0x20, 0x02), TAG, "oled page addressing");
    ESP_RETURN_ON_ERROR(oled_cmd(0xA1), TAG, "oled segment remap");
    ESP_RETURN_ON_ERROR(oled_cmd(0xC8), TAG, "oled com scan");
    ESP_RETURN_ON_ERROR(oled_cmd2(0xDA, OLED_COM_PINS), TAG, "oled com pins");
    ESP_RETURN_ON_ERROR(oled_cmd2(0x81, 0x7F), TAG, "oled contrast");
    ESP_RETURN_ON_ERROR(oled_cmd2(0xD9, 0xF1), TAG, "oled precharge");
    ESP_RETURN_ON_ERROR(oled_cmd2(0xDB, 0x40), TAG, "oled vcom");
    ESP_RETURN_ON_ERROR(oled_cmd(0xA4), TAG, "oled resume ram");
    ESP_RETURN_ON_ERROR(oled_cmd(0xA6), TAG, "oled normal display");
    ESP_RETURN_ON_ERROR(oled_clear(), TAG, "oled clear");
    ESP_RETURN_ON_ERROR(oled_cmd(0xAF), TAG, "oled display on");
    return ESP_OK;
}

static esp_err_t display_init_hw(void)
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

    esp_err_t err = lcd_init_driver();
    if (err == ESP_OK)
        return ESP_OK;

    display_available = false;
    active_display = NULL;
    ESP_LOGI(TAG, "HD44780 init failed: %s; trying SSD1306 OLED", esp_err_to_name(err));
    return oled_init_driver();
}

static void draw_connecting(uint32_t tick)
{
    char line1[LCD_COLS + 1];
    char line2[LCD_COLS + 1];
    snprintf(line1, sizeof(line1), "Hello!");
    snprintf(line2, sizeof(line2), "WiFi%.*s", (int)(tick % 4), "...");
    active_display->set_cursor(0, 0);
    active_display->write_padded(line1);
    active_display->set_cursor(1, 0);
    active_display->write_padded(line2);
    if (active_display->rows > 2)
    {
        active_display->set_cursor(2, 0);
        active_display->write_padded("");
    }
}

static void draw_status(void)
{
    display_state_t copy;
    portENTER_CRITICAL(&state_mux);
    copy = state;
    portEXIT_CRITICAL(&state_mux);

    char line1[LCD_COLS + 1];
    char line2[LCD_COLS + 1];
    char line3[LCD_COLS + 1];
    snprintf(line1, sizeof(line1), "tinyblok NATS %s",
             copy.nats_ip[0] != '\0' ? "OK" : "--");
    snprintf(line2, sizeof(line2), "PUBs: %lu", (unsigned long)tinyblok_pub_count());
    snprintf(line3, sizeof(line3), "%s", copy.nats_ip[0] != '\0' ? copy.nats_ip : "");

    active_display->set_cursor(0, 0);
    active_display->write_padded(line1);
    active_display->set_cursor(1, 0);
    active_display->write_padded(line2);
    if (active_display->rows > 2)
    {
        active_display->set_cursor(2, 0);
        active_display->write_padded(line3);
    }
}

static void display_task(void *arg)
{
    (void)arg;
    display_task_handle = xTaskGetCurrentTaskHandle();

    esp_err_t err = display_init_hw();
    if (err != ESP_OK)
    {
        display_available = false;
        ESP_LOGW(TAG, "no HD44780 LCD or SSD1306 OLED detected: %s; continuing without display",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "%s ready", active_display->name);
    esp_err_t reg_err = esp_event_handler_register(TINYBLOK_EVENT, ESP_EVENT_ANY_ID,
                                                   &app_event_handler, NULL);
    if (reg_err != ESP_OK)
        ESP_LOGW(TAG, "display event registration failed: %s", esp_err_to_name(reg_err));

    active_display->clear();
    active_display->set_cursor(0, 0);
    char splash[LCD_COLS + 1];
    snprintf(splash, sizeof(splash), "tinyblok v%.6s", TINYBLOK_VERSION);
    active_display->write_padded(splash);
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
