/* ------------------------------------------------------------------------- *\
 * Standard Includes
 * ------------------------------------------------------------------------- */
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- *\
 * RTOS Includes
 * ------------------------------------------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/* ------------------------------------------------------------------------- *\
 * esp-idf Includes
 * ------------------------------------------------------------------------- */
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

/* ------------------------------------------------------------------------- *\
 * Common MbedCraft includes
 * ------------------------------------------------------------------------- */
#include "mc_assert.h"
#include "mc_wifi.h"

/* ------------------------------------------------------------------------- *\
 * Defines
 * ------------------------------------------------------------------------- */
#define JOIN_TIMEOUT_MS (10000)
#define CONNECTED_BIT BIT0

/* ------------------------------------------------------------------------- *\
 * Constants
 * ------------------------------------------------------------------------- */
static const uint8_t _mc_soft_ap_ssid[] = CONFIG_MC_SOFT_AP_SSID;
static const uint8_t _mc_soft_ap_password[] = CONFIG_MC_SOFT_AP_PASSWORD;

/* ------------------------------------------------------------------------- *\
 * Static variables
 * ------------------------------------------------------------------------- */
static EventGroupHandle_t wifi_event_group;

/* ------------------------------------------------------------------------- *\
 * Internal functions prototypes
 * ------------------------------------------------------------------------- */
static void _sta_event_handler(
        void* arg, esp_event_base_t event_base,
        int32_t event_id, void* event_data);
static void _soft_ap_event_handler(
        void* arg, esp_event_base_t event_base,
        int32_t event_id, void* event_data);
static int _store_credentials(
        const uint8_t * const ssid, size_t ssid_size,
        const uint8_t * const password, size_t password_size);
static int _get_credentials(
        uint8_t * const ssid, size_t * ssid_size,
        uint8_t * const password, size_t * pass_size);
static mc_wifi_err_t _initialize_wifi(void);
static mc_wifi_err_t _try_connect_from_credentials(void);
static mc_wifi_err_t _start_soft_ap_server(void);
static mc_wifi_err_t _wifi_join_internal(
        const uint8_t * const ssid, size_t ssid_size,
        const uint8_t * const password, size_t password_size,
        int timeout_ms);

/* ------------------------------------------------------------------------- *\
 * Public function implementations
 * ------------------------------------------------------------------------- */
mc_wifi_err_t mc_wifi_start_soft_ap(
        const uint8_t * const ssid, size_t ssid_size,
        const uint8_t * const password, size_t password_size,
        int channel) {
    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = ssid_size,
            .channel = channel,
            .max_connection = CONFIG_MC_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    assert(ssid_size < sizeof(wifi_config.ap.ssid));
    assert(password_size < sizeof(wifi_config.ap.password));

    memcpy(wifi_config.ap.ssid, ssid, ssid_size);
    memcpy(wifi_config.ap.password, password, password_size);

    if (password_size == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(__func__, "wifi_init_softap finished. SSID:%.*s password:%.*s channel:%d",
            ssid_size, ssid, password_size, password, channel);


    return mc_wifi_ok;
}

mc_wifi_err_t mc_wifi_join(
        const uint8_t * const ssid, size_t ssid_size,
        const uint8_t * const password, size_t password_size,
        int timeout) {
    /* set default value*/
    if (timeout == 0) {
        timeout = JOIN_TIMEOUT_MS;
    }

    mc_wifi_err_t ret = _wifi_join_internal(
            ssid, ssid_size,
            password, password_size,
            timeout);

    if (ret != mc_wifi_ok) {
        ESP_LOGW(__func__, "Connection timed out");
        return ret;
    }

    ESP_LOGI(__func__, "Connected");

    _store_credentials(ssid, ssid_size, password, password_size);

    return mc_wifi_ok;
}

void mc_wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();

    assert(mc_wifi_ok == _initialize_wifi());

    if (mc_wifi_ok != _try_connect_from_credentials()) {
        _start_soft_ap_server();
    }
}

/* ------------------------------------------------------------------------- *\
 * Internal function implementations
 * ------------------------------------------------------------------------- */
static mc_wifi_err_t _try_connect_from_credentials(void) {
    uint8_t ssid[32];
    uint8_t password[64];
    size_t ssid_size = sizeof(ssid);
    size_t password_size = sizeof(password);
    int ret;

    ret = _get_credentials( ssid, &ssid_size, password, &password_size);

    if (ESP_OK == ret) {
        _wifi_join_internal(
                ssid, ssid_size,
                password, password_size,
                JOIN_TIMEOUT_MS);
        return mc_wifi_ok;
    } else {
        return mc_wifi_credentials_not_found;
    }
}

static mc_wifi_err_t _start_soft_ap_server(void) {
    mc_wifi_start_soft_ap(
            /* Does not count the '\0' character of the SSID*/
            _mc_soft_ap_ssid, sizeof(_mc_soft_ap_ssid) - 1,
            _mc_soft_ap_password, sizeof(_mc_soft_ap_password),
            CONFIG_MC_WIFI_CHANNEL);

    /* TODO */

    return mc_wifi_ok;
}

static mc_wifi_err_t _initialize_wifi(void)
{
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );

    ESP_ERROR_CHECK( esp_event_handler_instance_register(
                WIFI_EVENT,
                WIFI_EVENT_AP_STACONNECTED,
                &_soft_ap_event_handler,
                NULL,
                NULL) );

    ESP_ERROR_CHECK( esp_event_handler_instance_register(
                WIFI_EVENT,
                WIFI_EVENT_AP_STADISCONNECTED,
                &_soft_ap_event_handler,
                NULL,
                NULL) );

    ESP_ERROR_CHECK( esp_event_handler_instance_register(
                WIFI_EVENT,
                WIFI_EVENT_STA_DISCONNECTED,
                &_sta_event_handler,
                NULL,
                NULL) );

    ESP_ERROR_CHECK( esp_event_handler_instance_register(
                IP_EVENT,
                IP_EVENT_STA_GOT_IP,
                &_sta_event_handler,
                NULL,
                NULL) );

    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_NULL) );
    ESP_ERROR_CHECK( esp_wifi_start() );

    return mc_wifi_ok;
}

static void _soft_ap_event_handler(void* arg, esp_event_base_t event_base,
        int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(__func__, "station "MACSTR" join, AID=%d",
                MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(__func__, "station "MACSTR" leave, AID=%d",
                MAC2STR(event->mac), event->aid);
    }
}

static void _sta_event_handler(void* arg, esp_event_base_t event_base,
        int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    }
}

static mc_wifi_err_t _wifi_join_internal(
        const uint8_t * const ssid, size_t ssid_size,
        const uint8_t * const password, size_t password_size,
        int timeout_ms) {
    wifi_config_t wifi_config = { 0 };

    ESP_LOGI(__func__, "Connecting to '%.*s'", ssid_size, ssid);

    assert(ssid_size <= sizeof(wifi_config.sta.ssid));
    assert(password_size <= sizeof(wifi_config.sta.password));

    memcpy(wifi_config.sta.ssid, ssid, ssid_size);

    if (NULL != password) {
        memset(wifi_config.sta.password, 0, sizeof(wifi_config.sta.password));
        memcpy(wifi_config.sta.password, password, password_size);
    }

    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );

    tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, HOSTNAME);

    ESP_ERROR_CHECK( esp_wifi_connect() );

    int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                                   pdFALSE, pdTRUE, timeout_ms / portTICK_PERIOD_MS);

    if (bits & CONNECTED_BIT) {
        return mc_wifi_ok;
    } else {
        return mc_wifi_join_failed;
    }
}

static int _get_credentials(
        uint8_t * const ssid, size_t * ssid_size,
        uint8_t * const password, size_t * pass_size) {
    esp_err_t err;

    nvs_handle nvs_handle;
    err = nvs_open("wifi", NVS_READWRITE, &nvs_handle);
    ASSERT_RET(err == ESP_OK, err, "failed to open storage");

    do {
        ESP_LOGI(__func__, "reading SSID");
        err = nvs_get_blob(nvs_handle, "ssid", ssid, ssid_size);
        ASSERT_BREAK(err == ESP_OK, "failed to read SSID");

        ESP_LOGI(__func__, "reading password");
        err = nvs_get_blob(nvs_handle, "password", password, pass_size);
        ASSERT_BREAK(err == ESP_OK, "failed to read password");
    } while (0);

    nvs_close(nvs_handle);

    return err;
}

static int _store_credentials(
        const uint8_t * const ssid, size_t ssid_size,
        const uint8_t * const password, size_t password_size) {
    esp_err_t err;

    // Store credentials in NVS
    ESP_LOGI(__func__, "Storing credentials in NVS");

    nvs_handle nvs_handle;
    err = nvs_open("wifi", NVS_READWRITE, &nvs_handle);
    ASSERT_RET(err == ESP_OK, err, "failed to open storage");

    do {
        ESP_LOGI(__func__, "writing SSID");
        err = nvs_set_blob(nvs_handle, "ssid", ssid, ssid_size);
        ASSERT_BREAK(err == ESP_OK, "failed to write SSID");

        ESP_LOGI(__func__, "writing password");
        err = nvs_set_blob(nvs_handle, "password", password, password_size);
        ASSERT_BREAK(err == ESP_OK, "failed to write password");

        ESP_LOGI(__func__, "committing values");
        err = nvs_commit(nvs_handle);
        ASSERT_BREAK(err == ESP_OK, "failed to commit");

    } while (0);

    nvs_close(nvs_handle);

    return err;
}

