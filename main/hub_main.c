#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define HUB_UART              UART_NUM_0
#define HUB_UART_BAUD         115200
#define HUB_CHANNEL           1
#define HUB_MAX_NODES         6
#define HUB_LINE_SIZE         128
#define HUB_FRAME_MAGIC       0xA7
#define HUB_FRAME_VERSION     1
#define HUB_FRAME_HEADER_SIZE 9
#define HUB_FRAME_TELEMETRY   1
#define HUB_FRAME_COMMAND     16
#define HUB_CMD_POLL          1
#define HUB_TELEMETRY_SIZE    24
#define HUB_NVS_NAMESPACE    "hub_demo"
#define HUB_NVS_KEY_NODES    "nodes"
#define HUB_POLL_TIMEOUT_MS  700
#define HUB_POLL_CYCLE_MS    10

static const char *TAG = "hub_demo";

typedef struct {
    uint8_t mac[6];
    uint8_t data[250];
    uint16_t len;
} rx_packet_t;

static uint8_t s_nodes[HUB_MAX_NODES][6];
static size_t s_node_count;
static uint16_t s_next_sequence = 1;
static QueueHandle_t s_rx_queue;
static SemaphoreHandle_t s_poll_mutex;
static volatile bool s_polling_enabled;
static portMUX_TYPE s_nodes_mux = portMUX_INITIALIZER_UNLOCKED;

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float read_f32(const uint8_t *p)
{
    uint32_t bits = read_u32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void write_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void format_mac(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_mac(const char *text, uint8_t mac[6])
{
    if (text == NULL || strlen(text) != 17) return false;
    for (size_t i = 0; i < 6; ++i) {
        size_t pos = i * 3;
        int hi = hex_value(text[pos]);
        int lo = hex_value(text[pos + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && text[pos + 2] != ':')) return false;
        mac[i] = (uint8_t)((hi << 4) | lo);
    }
    bool zero = true;
    for (size_t i = 0; i < 6; ++i) zero = zero && mac[i] == 0;
    return !zero && (mac[0] & 1U) == 0;
}

static int find_node(const uint8_t mac[6])
{
    for (size_t i = 0; i < s_node_count; ++i)
        if (memcmp(s_nodes[i], mac, 6) == 0) return (int)i;
    return -1;
}

static void save_nodes(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(HUB_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        printf("ERR nvs_open %s\r\n", esp_err_to_name(err));
        return;
    }
    if (s_node_count == 0) {
        err = nvs_erase_key(handle, HUB_NVS_KEY_NODES);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_blob(handle, HUB_NVS_KEY_NODES, s_nodes, s_node_count * 6);
    }
    if (err == ESP_OK) err = nvs_set_u8(handle, "count", (uint8_t)s_node_count);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) printf("ERR save_nodes %s\r\n", esp_err_to_name(err));
}

static void load_nodes(void)
{
    nvs_handle_t handle;
    if (nvs_open(HUB_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t count = 0;
    size_t bytes = sizeof(s_nodes);
    esp_err_t err = nvs_get_u8(handle, "count", &count);
    if (err == ESP_OK && count <= HUB_MAX_NODES) {
        if (count == 0) {
            s_node_count = 0;
        } else if (nvs_get_blob(handle, HUB_NVS_KEY_NODES, s_nodes, &bytes) == ESP_OK && bytes == count * 6) {
            s_node_count = count;
        }
    }
    nvs_close(handle);
}

static void on_receive(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || info->src_addr == NULL || data == NULL || len < HUB_FRAME_HEADER_SIZE || len > 250)
        return;
    rx_packet_t packet = { .len = (uint16_t)len };
    memcpy(packet.mac, info->src_addr, 6);
    memcpy(packet.data, data, len);
    (void)xQueueSend(s_rx_queue, &packet, 0);
}

static esp_err_t add_peer(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) return ESP_OK;
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = HUB_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer);
}

static void print_telemetry(const uint8_t mac[6], const uint8_t *payload)
{
    char mac_text[18];
    format_mac(mac, mac_text);
    uint32_t uptime = read_u32(payload);
    int16_t adc = (int16_t)read_u16(payload + 4);
    float pressure = read_f32(payload + 6);
    float setpoint = read_f32(payload + 10);
    float volume_raw = read_f32(payload + 14);
    float volume_scaled = read_f32(payload + 18);
    uint16_t flags = read_u16(payload + 22);

    /* Machine-readable line for the PC, followed by the raw wire payload. */
    printf("{\"type\":\"telemetry\",\"mac\":\"%s\",\"uptime_ms\":%" PRIu32
           ",\"adc\":%d,\"pressure_kpa\":%.4f,\"setpoint_kpa\":%.4f"
           ",\"volume_raw\":%.4f,\"volume_scaled\":%.4f,\"flags\":%u}\r\n",
           mac_text, uptime, (int)adc, (double)pressure, (double)setpoint,
           (double)volume_raw, (double)volume_scaled, (unsigned)flags);
}

static bool poll_node(const uint8_t mac[6])
{
    if (xSemaphoreTake(s_poll_mutex, portMAX_DELAY) != pdTRUE) return false;
    uint16_t sequence = s_next_sequence++;
    uint8_t frame[HUB_FRAME_HEADER_SIZE + 1] = {
        HUB_FRAME_MAGIC, HUB_FRAME_VERSION, HUB_FRAME_COMMAND, 0, 1, 0, 0, 1, 0, HUB_CMD_POLL
    };
    write_u16(frame + 5, sequence);

    /* Drop stale frames: each poll has a unique sequence ID. */
    rx_packet_t stale;
    while (xQueueReceive(s_rx_queue, &stale, 0) == pdTRUE) {}

    esp_err_t err = esp_now_send(mac, frame, sizeof(frame));
    char mac_text[18];
    format_mac(mac, mac_text);
    if (err != ESP_OK) {
        printf("{\"type\":\"error\",\"mac\":\"%s\",\"stage\":\"send\",\"error\":\"%s\"}\r\n",
               mac_text, esp_err_to_name(err));
        xSemaphoreGive(s_poll_mutex);
        return false;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(HUB_POLL_TIMEOUT_MS);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        rx_packet_t packet;
        TickType_t remaining = deadline - xTaskGetTickCount();
        if (xQueueReceive(s_rx_queue, &packet, remaining) != pdTRUE) break;

        const uint8_t *p = packet.data;
        if (memcmp(packet.mac, mac, 6) != 0 || p[0] != HUB_FRAME_MAGIC ||
            p[1] != HUB_FRAME_VERSION || p[2] != HUB_FRAME_TELEMETRY ||
            p[3] != 0 || p[4] != 1 || read_u16(p + 5) != sequence ||
            read_u16(p + 7) != HUB_TELEMETRY_SIZE ||
            packet.len != HUB_FRAME_HEADER_SIZE + HUB_TELEMETRY_SIZE) continue;

        print_telemetry(mac, p + HUB_FRAME_HEADER_SIZE);
        xSemaphoreGive(s_poll_mutex);
        return true;
    }
    printf("{\"type\":\"timeout\",\"mac\":\"%s\",\"request_id\":%u}\r\n",
           mac_text, (unsigned)sequence);
    xSemaphoreGive(s_poll_mutex);
    return false;
}

/* Polling runs independently from UART input; connecting the PC (list command)
 * starts a live stream without requiring a button press for each sample. */
static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_polling_enabled) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint8_t nodes[HUB_MAX_NODES][6];
        size_t count;
        taskENTER_CRITICAL(&s_nodes_mux);
        count = s_node_count;
        memcpy(nodes, s_nodes, count * sizeof(nodes[0]));
        taskEXIT_CRITICAL(&s_nodes_mux);

        for (size_t i = 0; i < count; ++i) {
            if (!s_polling_enabled) break;
            (void)poll_node(nodes[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(HUB_POLL_CYCLE_MS));
    }
}

static void print_help(void)
{
    printf("Hub ESP-NOW demo commands (UART 115200, newline):\r\n"
           "  add AA:BB:CC:DD:EE:FF   add/save a pump node (max 6)\r\n"
           "  del AA:BB:CC:DD:EE:FF   remove/save a node\r\n"
           "  list                   show configured nodes and hub MAC\r\n"
           "  poll AA:BB:CC:DD:EE:FF  request one telemetry sample\r\n"
           "  poll all               request one sample from each node in order\r\n"
           "  help\r\n"
           "Output: one JSON line per response/error, suitable for a PC parser.\r\n");
}

static void print_list(void)
{
    uint8_t hub_mac[6];
    char text[18];
    if (esp_wifi_get_mac(WIFI_IF_STA, hub_mac) == ESP_OK) {
        format_mac(hub_mac, text);
        printf("{\"type\":\"hub\",\"mac\":\"%s\",\"channel\":%d}\r\n", text, HUB_CHANNEL);
    }
    for (size_t i = 0; i < s_node_count; ++i) {
        format_mac(s_nodes[i], text);
        printf("{\"type\":\"node\",\"index\":%u,\"mac\":\"%s\"}\r\n",
               (unsigned)(i + 1), text);
    }
    if (s_node_count == 0) printf("{\"type\":\"nodes\",\"count\":0}\r\n");
}

static void handle_line(char *line)
{
    char *save = NULL;
    char *command = strtok_r(line, " \t", &save);
    char *argument = strtok_r(NULL, " \t", &save);
    if (command == NULL) return;

    if (strcmp(command, "help") == 0) {
        print_help();
    } else if (strcmp(command, "list") == 0) {
        s_polling_enabled = true;
        print_list();
    } else if (strcmp(command, "add") == 0 || strcmp(command, "del") == 0) {
        uint8_t mac[6];
        if (!parse_mac(argument, mac)) {
            printf("{\"type\":\"error\",\"message\":\"invalid MAC; expected AA:BB:CC:DD:EE:FF\"}\r\n");
            return;
        }
        int index = find_node(mac);
        if (strcmp(command, "add") == 0) {
            if (index >= 0) { printf("{\"type\":\"ok\",\"message\":\"node already exists\"}\r\n"); return; }
            if (s_node_count >= HUB_MAX_NODES) { printf("{\"type\":\"error\",\"message\":\"node list full\"}\r\n"); return; }
            esp_err_t err = add_peer(mac);
            if (err != ESP_OK) {
                printf("{\"type\":\"error\",\"stage\":\"add_peer\",\"error\":\"%s\"}\r\n", esp_err_to_name(err));
                return;
            }
            taskENTER_CRITICAL(&s_nodes_mux);
            memcpy(s_nodes[s_node_count++], mac, 6);
            taskEXIT_CRITICAL(&s_nodes_mux);
        } else {
            if (index < 0) { printf("{\"type\":\"ok\",\"message\":\"node not configured\"}\r\n"); return; }
            (void)esp_now_del_peer(mac);
            taskENTER_CRITICAL(&s_nodes_mux);
            for (size_t i = (size_t)index; i + 1 < s_node_count; ++i)
                memcpy(s_nodes[i], s_nodes[i + 1], 6);
            --s_node_count;
            taskEXIT_CRITICAL(&s_nodes_mux);
        }
        save_nodes();
        printf("{\"type\":\"ok\",\"command\":\"%s\"}\r\n", command);
    } else if (strcmp(command, "poll") == 0) {
        if (argument == NULL) { print_help(); return; }
        if (strcmp(argument, "all") == 0) {
            for (size_t i = 0; i < s_node_count; ++i) (void)poll_node(s_nodes[i]);
        } else {
            uint8_t mac[6];
            if (!parse_mac(argument, mac) || find_node(mac) < 0) {
                printf("{\"type\":\"error\",\"message\":\"node is invalid or not configured\"}\r\n");
                return;
            }
            (void)poll_node(mac);
        }
    } else {
        printf("{\"type\":\"error\",\"message\":\"unknown command; send help\"}\r\n");
    }
}

static void uart_task(void *arg)
{
    (void)arg;
    char line[HUB_LINE_SIZE];
    size_t used = 0;
    uint8_t byte;
    printf("READY hub ESP-NOW channel=%d uart=%d baud=%d\r\n", HUB_CHANNEL, HUB_UART, HUB_UART_BAUD);
    print_help();
    for (;;) {
        int count = uart_read_bytes(HUB_UART, &byte, 1, pdMS_TO_TICKS(100));
        if (count <= 0) continue;
        if (byte == '\r') continue;
        if (byte == '\n') {
            line[used] = '\0';
            handle_line(line);
            used = 0;
        } else if (used + 1 < sizeof(line)) {
            line[used++] = (char)byte;
        } else {
            used = 0;
            printf("{\"type\":\"error\",\"message\":\"command too long\"}\r\n");
        }
    }
}

static void start_uart(void)
{
    uart_config_t config = {
        .baud_rate = HUB_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(HUB_UART, 2048, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(HUB_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(HUB_UART, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void start_espnow(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(HUB_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_receive));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    s_rx_queue = xQueueCreate(12, sizeof(rx_packet_t));
    if (s_rx_queue == NULL) {
        ESP_LOGE(TAG, "cannot create RX queue");
        abort();
    }
    s_poll_mutex = xSemaphoreCreateMutex();
    if (s_poll_mutex == NULL) {
        ESP_LOGE(TAG, "cannot create poll mutex");
        abort();
    }
    start_uart();
    start_espnow();
    load_nodes();
    for (size_t i = 0; i < s_node_count; ++i) {
        err = add_peer(s_nodes[i]);
        if (err != ESP_OK) ESP_LOGE(TAG, "restore peer failed: %s", esp_err_to_name(err));
    }

    xTaskCreate(uart_task, "hub_uart", 4096, NULL, 5, NULL);
    xTaskCreate(poll_task, "hub_poll", 4096, NULL, 4, NULL);
}
