/*
 *   This file is part of DroneBridge: https://github.com/DroneBridge/ESP32
 *
 *   Copyright 2018 Wolfgang Christl
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */

#include <stdio.h>
#include <nvs_flash.h>
#include <esp_wifi_types.h>
#include <mdns.h>
#include <string.h>
#include <driver/gpio.h>
#include <lwip/apps/netbiosns.h>
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_modem_api.h"
#include "esp_log.h"
#include "esp_event.h"
#include "db_esp32_control.h"
#include "http_server.h"
#include "db_esp32_comm.h"
#include "db_protocol.h"
#include "esp_vfs_semihost.h"
#include "esp_spiffs.h"
#include "http_server.h"
#include "globals.h"
#include "main.h"

#define NVS_NAMESPACE "settings"
#define USE_ALT_UART_CONFIG // for boards that have flash connected to GPIO 17/16 - will crash otherwise

static const int CONNECT_BIT = BIT0;
static const int GOT_DATA_BIT = BIT2;

#if defined(CONFIG_EXAMPLE_FLOW_CONTROL_NONE)
#define EXAMPLE_FLOW_CONTROL ESP_MODEM_FLOW_CONTROL_NONE
#elif defined(CONFIG_EXAMPLE_FLOW_CONTROL_SW)
#define EXAMPLE_FLOW_CONTROL ESP_MODEM_FLOW_CONTROL_SW
#elif defined(CONFIG_EXAMPLE_FLOW_CONTROL_HW)
#define EXAMPLE_FLOW_CONTROL ESP_MODEM_FLOW_CONTROL_HW
#endif

#define HIGH 1
#define LOW 0

static EventGroupHandle_t event_group = NULL;

static const char *TAG = "DB_ESP32";

uint8_t DEFAULT_SSID[32] = "DroneBridge ESP32";
uint8_t DEFAULT_PWD[64] = "dronebridge";
char DEFAULT_AP_IP[32] = "192.168.2.1";
char INET_SERVER_IP[32] = CONFIG_MODEM_INET_SERVER_IP;
int INET_SERVER_PORT = CONFIG_MODEM_INET_SERVER_PORT;
uint8_t DEFAULT_CHANNEL = 6;
uint8_t SERIAL_PROTOCOL = 4;  // 1=MSP, 4=MAVLink/transparent
# ifdef USE_ALT_UART_CONFIG
uint8_t DB_UART_PIN_TX = GPIO_NUM_33;
uint8_t DB_UART_PIN_RX = GPIO_NUM_32;
# else
uint8_t DB_UART_PIN_TX = GPIO_NUM_17;
uint8_t DB_UART_PIN_RX = GPIO_NUM_16;
#endif
int DB_UART_BAUD_RATE = 115200;
uint16_t TRANSPARENT_BUF_SIZE = 64;
uint8_t LTM_FRAME_NUM_BUFFER = 1;
uint8_t MSP_LTM_SAMEPORT = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG, "Client connected - station:"MACSTR", AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG, "Client disconnected - station:"MACSTR", AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP started!");
    } else if (event_id == WIFI_EVENT_AP_STOP) {
        ESP_LOGI(TAG, "AP stopped!");
    }
}

void start_mdns_service() {
    //initialize mDNS service
    esp_err_t err = mdns_init();
    if (err) {
        printf("MDNS Init failed: %d\n", err);
        return;
    }
    ESP_ERROR_CHECK(mdns_hostname_set("dronebridge"));
    ESP_ERROR_CHECK(mdns_instance_name_set("DroneBridge for ESP32"));

    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_db_proxy", "_tcp", APP_PORT_PROXY, NULL, 0));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_db_comm", "_tcp", APP_PORT_COMM, NULL, 0));
    ESP_ERROR_CHECK(mdns_service_instance_name_set("_http", "_tcp", "DroneBridge for ESP32"));
}

#if CONFIG_WEB_DEPLOY_SEMIHOST
esp_err_t init_fs(void) {
    esp_err_t ret = esp_vfs_semihost_register(CONFIG_WEB_MOUNT_POINT, CONFIG_HOST_PATH_TO_MOUNT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register semihost driver (%s)!", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    return ESP_OK;
}
#endif


#if CONFIG_WEB_DEPLOY_SF

esp_err_t init_fs(void) {
    esp_vfs_spiffs_conf_t conf = {
            .base_path = CONFIG_WEB_MOUNT_POINT,
            .partition_label = NULL,
            .max_files = 5,
            .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ESP_OK;
}

#endif

void init_wifi(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *esp_net = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
            .ap = {
                    .ssid = "DroneBridge_ESP32_Init",
                    .ssid_len = 0,
                    .authmode = WIFI_AUTH_WPA_PSK,
                    .channel = DEFAULT_CHANNEL,
                    .ssid_hidden = 0,
                    .beacon_interval = 100,
                    .max_connection = 10
            },
    };
    xthal_memcpy(wifi_config.ap.ssid, DEFAULT_SSID, 32);
    xthal_memcpy(wifi_config.ap.password, DEFAULT_PWD, 64);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    wifi_country_t wifi_country = {.cc = "US", .schan = 1, .nchan = 13, .policy = WIFI_COUNTRY_POLICY_MANUAL};
    ESP_ERROR_CHECK(esp_wifi_set_country(&wifi_country));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(esp_netif_ip_info_t));
    ip.ip.addr = ipaddr_addr(DEFAULT_AP_IP);
    ip.netmask.addr = ipaddr_addr("255.255.255.0");
    ip.gw.addr = ipaddr_addr(DEFAULT_AP_IP);
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(esp_net));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(esp_net, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(esp_net));

    ESP_ERROR_CHECK(esp_netif_set_hostname(esp_net, "DBESP32"));
}

static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "PPP state changed event %li", event_id);
    if (event_id == NETIF_PPP_ERRORUSER) {
        /* User interrupted event from esp-netif */
        esp_netif_t *netif = event_data;
        ESP_LOGI(TAG, "User interrupted event from netif:%p", netif);
    }
}


static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "IP event! %li", event_id);
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        esp_netif_dns_info_t dns_info;

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_t *netif = event->esp_netif;

        ESP_LOGI(TAG, "Modem Connect to PPP Server");
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));
        esp_netif_get_dns_info(netif, 0, &dns_info);
        ESP_LOGI(TAG, "Name Server1: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        esp_netif_get_dns_info(netif, 1, &dns_info);
        ESP_LOGI(TAG, "Name Server2: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        xEventGroupSetBits(event_group, CONNECT_BIT);

        ESP_LOGI(TAG, "GOT ip event!!!");
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGI(TAG, "Modem Disconnect from PPP Server");
    } else if (event_id == IP_EVENT_GOT_IP6) {
        ESP_LOGI(TAG, "GOT IPv6 event!");

        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        ESP_LOGI(TAG, "Got IPv6 address " IPV6STR, IPV62STR(event->ip6_info.ip));
    }
}

void init_modem() {
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));

    gpio_set_direction(CONFIG_MODEM_POWER_PIN,GPIO_MODE_OUTPUT);
    gpio_set_direction(CONFIG_MODEM_RESET_PIN,GPIO_MODE_OUTPUT);
    gpio_set_direction(CONFIG_MODEM_PWKEY_PIN,GPIO_MODE_OUTPUT);

    gpio_set_level(CONFIG_MODEM_RESET_PIN,HIGH);
    vTaskDelay(1000 /portTICK_PERIOD_MS);
    gpio_set_level(CONFIG_MODEM_PWKEY_PIN,HIGH);
    vTaskDelay(1000 /portTICK_PERIOD_MS);
    gpio_set_level(CONFIG_MODEM_POWER_PIN,HIGH);
    vTaskDelay(1000 /portTICK_PERIOD_MS);
    gpio_set_level(CONFIG_MODEM_PWKEY_PIN,LOW);
    vTaskDelay(1000 /portTICK_PERIOD_MS);

    /* Configure the PPP netif */
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_MODEM_PPP_APN);
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&netif_ppp_config);
    assert(esp_netif);

    event_group = xEventGroupCreate();

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    /* setup UART specific configuration based on kconfig options */
    dte_config.uart_config.tx_io_num = CONFIG_MODEM_UART_TX_PIN;
    dte_config.uart_config.rx_io_num = CONFIG_MODEM_UART_RX_PIN;

    ESP_LOGI(TAG, "Initializing esp_modem for the SIM800 module...");
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM800, &dte_config, &dce_config, esp_netif);

    xEventGroupClearBits(event_group, CONNECT_BIT | GOT_DATA_BIT);

    vTaskDelay(5000 /portTICK_PERIOD_MS);

    esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_modem_set_mode(ESP_MODEM_MODE_DATA) failed with %d", err);
        return;
    }
    /* Wait for IP address */
    ESP_LOGI(TAG, "Waiting for IP address");
    xEventGroupWaitBits(event_group, CONNECT_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

void write_settings_to_nvs() {
    ESP_LOGI(TAG,
             "Trying to save: ssid %s\nwifi_pass %s\nwifi_chan %i\nbaud %i\ngpio_tx %i\ngpio_rx %i\nproto %i\n"
             "trans_pack_size %i\nltm_per_packet %i\nmsp_ltm %i\nap_ip %s\ninet_server_ip %s\ninet_srv_port%i",
             DEFAULT_SSID, DEFAULT_PWD, DEFAULT_CHANNEL, DB_UART_BAUD_RATE, DB_UART_PIN_TX, DB_UART_PIN_RX,
             SERIAL_PROTOCOL, TRANSPARENT_BUF_SIZE, LTM_FRAME_NUM_BUFFER, MSP_LTM_SAMEPORT, DEFAULT_AP_IP,
             INET_SERVER_IP,INET_SERVER_PORT);
    ESP_LOGI(TAG, "Saving to NVS %s", NVS_NAMESPACE);
    nvs_handle my_handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle));
    ESP_ERROR_CHECK(nvs_set_str(my_handle, "ssid", (char *) DEFAULT_SSID));
    ESP_ERROR_CHECK(nvs_set_str(my_handle, "wifi_pass", (char *) DEFAULT_PWD));
    ESP_ERROR_CHECK(nvs_set_str(my_handle, "inet_server_ip", (char *) INET_SERVER_IP));
    ESP_ERROR_CHECK(nvs_set_i32(my_handle, "inet_srv_port", INET_SERVER_PORT));
    ESP_ERROR_CHECK(nvs_set_u8(my_handle, "wifi_chan", DEFAULT_CHANNEL));
    ESP_ERROR_CHECK(nvs_set_i32(my_handle, "baud", DB_UART_BAUD_RATE));
    ESP_ERROR_CHECK(nvs_set_u8(my_handle, "gpio_tx", DB_UART_PIN_TX));
    ESP_ERROR_CHECK(nvs_set_u8(my_handle, "gpio_rx", DB_UART_PIN_RX));
    ESP_ERROR_CHECK(nvs_set_u8(my_handle, "proto", SERIAL_PROTOCOL));
    ESP_ERROR_CHECK(nvs_set_u16(my_handle, "trans_pack_size", TRANSPARENT_BUF_SIZE));
    ESP_ERROR_CHECK(nvs_set_u8(my_handle, "ltm_per_packet", LTM_FRAME_NUM_BUFFER));
    ESP_ERROR_CHECK(nvs_set_u8(my_handle, "msp_ltm", MSP_LTM_SAMEPORT));
    ESP_ERROR_CHECK(nvs_set_str(my_handle, "ap_ip", DEFAULT_AP_IP));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
    nvs_close(my_handle);
}


void read_settings_nvs() {
    nvs_handle my_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle) != ESP_OK) {
        // First start
        nvs_close(my_handle);
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
        write_settings_to_nvs();
    } else {
        ESP_LOGI(TAG, "Reading settings from NVS");
        size_t required_size = 0;
        ESP_ERROR_CHECK(nvs_get_str(my_handle, "ssid", NULL, &required_size));
        char *ssid = malloc(required_size);
        ESP_ERROR_CHECK(nvs_get_str(my_handle, "ssid", ssid, &required_size));
        memcpy(DEFAULT_SSID, ssid, required_size);

        ESP_ERROR_CHECK(nvs_get_str(my_handle, "wifi_pass", NULL, &required_size));
        char *wifi_pass = malloc(required_size);
        ESP_ERROR_CHECK(nvs_get_str(my_handle, "wifi_pass", wifi_pass, &required_size));
        memcpy(DEFAULT_PWD, wifi_pass, required_size);

        ESP_ERROR_CHECK(nvs_get_str(my_handle, "ap_ip", NULL, &required_size));
        char *ap_ip = malloc(required_size);
        ESP_ERROR_CHECK(nvs_get_str(my_handle, "ap_ip", ap_ip, &required_size));
        memcpy(DEFAULT_AP_IP, ap_ip, required_size);

        ESP_ERROR_CHECK(nvs_get_str(my_handle, "inet_server_ip", NULL, &required_size));
        char *inet_server_ip = malloc(required_size);
        ESP_ERROR_CHECK(nvs_get_str(my_handle, "inet_server_ip", inet_server_ip, &required_size));
        memcpy(INET_SERVER_IP, inet_server_ip, required_size);

        ESP_ERROR_CHECK(nvs_get_i32(my_handle, "inet_srv_port", &INET_SERVER_PORT));

        ESP_ERROR_CHECK(nvs_get_u8(my_handle, "wifi_chan", &DEFAULT_CHANNEL));
        ESP_ERROR_CHECK(nvs_get_i32(my_handle, "baud", &DB_UART_BAUD_RATE));
        ESP_ERROR_CHECK(nvs_get_u8(my_handle, "gpio_tx", &DB_UART_PIN_TX));
        ESP_ERROR_CHECK(nvs_get_u8(my_handle, "gpio_rx", &DB_UART_PIN_RX));
        ESP_ERROR_CHECK(nvs_get_u8(my_handle, "proto", &SERIAL_PROTOCOL));
        ESP_ERROR_CHECK(nvs_get_u16(my_handle, "trans_pack_size", &TRANSPARENT_BUF_SIZE));
        ESP_ERROR_CHECK(nvs_get_u8(my_handle, "ltm_per_packet", &LTM_FRAME_NUM_BUFFER));
        ESP_ERROR_CHECK(nvs_get_u8(my_handle, "msp_ltm", &MSP_LTM_SAMEPORT));

        nvs_close(my_handle);
        free(wifi_pass);
        free(ssid);
        free(ap_ip);
    }
}

void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    read_settings_nvs();
    esp_log_level_set("*", ESP_LOG_INFO);
    init_wifi();
    init_modem();
    start_mdns_service();
    netbiosns_init();
    netbiosns_set_name("dronebridge");

    ESP_ERROR_CHECK(init_fs());

    control_module();
    ESP_ERROR_CHECK(start_rest_server(CONFIG_WEB_MOUNT_POINT));
    communication_module();
}