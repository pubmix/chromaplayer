#include "connectivity.h"

#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#endif

static const char *TAG = "connectivity";
static ConnectivityWifiAP aps[CONNECTIVITY_MAX_WIFI];
static size_t ap_count;
static volatile bool scanning;
static volatile bool connected;
static volatile bool connecting;
static char connected_ssid[33];
static char wifi_status[48] = "Not connected";
static ConnectivityBluetoothDevice bt_devices[CONNECTIVITY_MAX_BT];
static size_t bt_count;
static volatile bool bt_scanning;
static volatile bool bt_connected;
static char bt_connected_name[33];
static StreamBufferHandle_t bt_audio;

#if CONFIG_BT_ENABLED
static void bt_name_from_props(esp_bt_gap_cb_param_t *param, char *name, size_t size, int8_t *rssi)
{
    name[0] = 0; *rssi = -127;
    for (int i = 0; i < param->disc_res.num_prop; ++i) {
        esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME && p->val) {
            size_t n = p->len < size - 1 ? (size_t)p->len : size - 1;
            memcpy(name, p->val, n); name[n] = 0;
        } else if (p->type == ESP_BT_GAP_DEV_PROP_RSSI && p->val) {
            *rssi = *(int8_t *)p->val;
        } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR && p->val && !name[0]) {
            uint8_t n = 0;
            uint8_t *s = esp_bt_gap_resolve_eir_data(p->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &n);
            if (!s) s = esp_bt_gap_resolve_eir_data(p->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &n);
            if (s) { size_t copy = n < size - 1 ? n : size - 1; memcpy(name, s, copy); name[copy] = 0; }
        }
    }
}

static void bt_gap_handler(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        for (size_t i = 0; i < bt_count; ++i)
            if (!memcmp(bt_devices[i].address, param->disc_res.bda, 6)) return;
        if (bt_count >= CONNECTIVITY_MAX_BT) return;
        ConnectivityBluetoothDevice *d = &bt_devices[bt_count++];
        memcpy(d->address, param->disc_res.bda, 6);
        bt_name_from_props(param, d->name, sizeof(d->name), &d->rssi);
        if (!d->name[0]) snprintf(d->name, sizeof(d->name), "%02X:%02X:%02X:%02X:%02X:%02X",
            d->address[0], d->address[1], d->address[2], d->address[3], d->address[4], d->address[5]);
    } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        bt_scanning = param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED;
    } else if (event == ESP_BT_GAP_CFM_REQ_EVT) {
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
    }
}

static int32_t bt_audio_callback(uint8_t *data, int32_t len)
{
    if (!data || len <= 0 || !bt_audio) return 0;
    size_t got = xStreamBufferReceive(bt_audio, data, (size_t)len, 0);
    if (got < (size_t)len) memset(data + got, 0, (size_t)len - got);
    return len;
}

static void bt_a2dp_handler(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (event == ESP_A2D_CONNECTION_STATE_EVT) {
        bt_connected = param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
        if (bt_connected) esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        else bt_connected_name[0] = 0;
    }
}

static void bluetooth_initialize(void)
{
    bt_audio = xStreamBufferCreate(32768, 1);
    if (!bt_audio) return;
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&cfg) != ESP_OK) return;
    if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) return;
    esp_bluedroid_config_t blue_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if (esp_bluedroid_init_with_cfg(&blue_cfg) != ESP_OK || esp_bluedroid_enable() != ESP_OK) return;
    esp_bt_gap_register_callback(bt_gap_handler);
    esp_bt_gap_set_device_name("ChromaPlayer");
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(iocap));
    esp_a2d_register_callback(bt_a2dp_handler);
    esp_a2d_source_register_data_callback(bt_audio_callback);
    esp_a2d_source_init();
}
#else
static void bluetooth_initialize(void) { }
#endif

static void start_time_sync(void)
{
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();
    if (esp_sntp_enabled()) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t count = CONNECTIVITY_MAX_WIFI;
        static wifi_ap_record_t records[CONNECTIVITY_MAX_WIFI];
        if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
            ap_count = count;
            for (size_t i = 0; i < ap_count; ++i) {
                strlcpy(aps[i].ssid, (const char *)records[i].ssid, sizeof(aps[i].ssid));
                aps[i].rssi = records[i].rssi;
                aps[i].secured = records[i].authmode != WIFI_AUTH_OPEN;
            }
        }
        scanning = false;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        connected = false;
        connecting = false;
        connected_ssid[0] = 0;
        snprintf(wifi_status, sizeof(wifi_status), "Failed (reason %u)",
                 event ? (unsigned)event->reason : 0U);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_config_t cfg = {0};
        esp_wifi_get_config(WIFI_IF_STA, &cfg);
        strlcpy(connected_ssid, (const char *)cfg.sta.ssid, sizeof(connected_ssid));
        connected = true;
        connecting = false;
        snprintf(wifi_status, sizeof(wifi_status), "Connected: %.32s", connected_ssid);
        start_time_sync();
    }
}

static void wifi_reconnect_task(void *unused)
{
    unsigned backoff = 2;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
        if (connected) { backoff = 2; continue; }
        if (connecting || scanning) continue;
        wifi_config_t saved = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &saved) != ESP_OK || !saved.sta.ssid[0]) continue;
        connecting = true;
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) connecting = false;
        backoff = backoff < 16 ? backoff * 2 : 30;
    }
}

void Connectivity_Initialize(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    wifi_config_t saved = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK && saved.sta.ssid[0]) {
        connecting = esp_wifi_connect() == ESP_OK;
    }
    xTaskCreate(wifi_reconnect_task, "wifi_retry", 3072, NULL, 3, NULL);
    bluetooth_initialize();
}

void Connectivity_BluetoothScan(void)
{
#if CONFIG_BT_ENABLED
    if (bt_scanning) return;
    bt_count = 0; bt_scanning = true;
    if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 8, 0) != ESP_OK) bt_scanning = false;
#endif
}
size_t Connectivity_BluetoothCount(void) { return bt_count; }
const ConnectivityBluetoothDevice *Connectivity_BluetoothGet(size_t i) { return i < bt_count ? &bt_devices[i] : NULL; }
bool Connectivity_BluetoothScanning(void) { return bt_scanning; }
bool Connectivity_BluetoothConnected(void) { return bt_connected; }
const char *Connectivity_BluetoothName(void) { return bt_connected_name; }
void Connectivity_BluetoothConnect(size_t i)
{
#if CONFIG_BT_ENABLED
    if (i >= bt_count) return;
    strlcpy(bt_connected_name, bt_devices[i].name, sizeof(bt_connected_name));
    esp_bt_gap_cancel_discovery();
    esp_a2d_source_connect(bt_devices[i].address);
#else
    (void)i;
#endif
}
size_t Connectivity_BluetoothAudioWrite(const void *data, size_t length)
{
    if (!bt_connected || !bt_audio) return 0;
    return xStreamBufferSend(bt_audio, data, length, pdMS_TO_TICKS(250));
}

void Connectivity_WifiScan(void)
{
    if (scanning) return;
    scanning = true;
    ap_count = 0;
    wifi_scan_config_t config = { .show_hidden = false };
    if (esp_wifi_scan_start(&config, false) != ESP_OK) scanning = false;
}

size_t Connectivity_WifiCount(void) { return ap_count; }
const ConnectivityWifiAP *Connectivity_WifiGet(size_t i) { return i < ap_count ? &aps[i] : NULL; }
bool Connectivity_WifiScanning(void) { return scanning; }
bool Connectivity_WifiConnected(void) { return connected; }
bool Connectivity_WifiConnecting(void) { return connecting; }
const char *Connectivity_WifiSSID(void) { return connected_ssid; }
const char *Connectivity_WifiStatus(void) { return wifi_status; }

void Connectivity_WifiConnect(const char *ssid, const char *password)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    connected = false;
    connecting = true;
    snprintf(wifi_status, sizeof(wifi_status), "Connecting: %.31s", ssid);
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());
}
