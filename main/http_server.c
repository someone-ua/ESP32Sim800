/*
 *   This file is part of DroneBridge: https://github.com/DroneBridge/ESP32
 *
 *   Copyright 2021 Wolfgang Christl
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

#include "http_server.h"
#include <string.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <esp_chip_info.h>
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "cJSON.h"
#include "globals.h"
#include "main.h"

static const char *REST_TAG = "DB_HTTP_REST";
#define REST_CHECK(a, str, goto_tag, ...)                                              \
    do                                                                                 \
    {                                                                                  \
        if (!(a))                                                                      \
        {                                                                              \
            ESP_LOGE(REST_TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                             \
        }                                                                              \
    } while (0)

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)

typedef struct rest_server_context {
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

bool is_valid_ip4(char *ipAddress) {
    struct sockaddr_in sa;
    int result = inet_pton(AF_INET, ipAddress, &(sa.sin_addr));
    return result != 0;
}

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filepath) {
    const char *type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        type = "text/xml";
    }
    return httpd_resp_set_type(req, type);
}

/* Send HTTP response with the contents of the requested file */
static esp_err_t rest_common_get_handler(httpd_req_t *req) {
    char filepath[FILE_PATH_MAX];

    rest_server_context_t *rest_context = (rest_server_context_t *) req->user_ctx;
    strlcpy(filepath, rest_context->base_path, sizeof(filepath));
    if (req->uri[strlen(req->uri) - 1] == '/') {
        strlcat(filepath, "/index.html", sizeof(filepath));
    } else {
        strlcat(filepath, req->uri, sizeof(filepath));
    }
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        ESP_LOGE(REST_TAG, "Failed to open file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filepath);

    char *chunk = rest_context->scratch;
    ssize_t read_bytes;
    do {
        /* Read file in chunks into the scratch buffer */
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
        if (read_bytes == -1) {
            ESP_LOGE(REST_TAG, "Failed to read file : %s", filepath);
        } else if (read_bytes > 0) {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                close(fd);
                ESP_LOGE(REST_TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_FAIL;
            }
        }
    } while (read_bytes > 0);
    /* Close file after sending complete */
    close(fd);
    ESP_LOGI(REST_TAG, "File sending complete");
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Simple handler for settings change */
static esp_err_t settings_change_post_handler(httpd_req_t *req) {
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = ((rest_server_context_t *) (req->user_ctx))->scratch;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *json = cJSON_GetObjectItem(root, "wifi_ssid");
    if (json && strlen(json->valuestring) < 32 && strlen(json->valuestring) > 0)
        strncpy((char *) DEFAULT_SSID, json->valuestring, sizeof(DEFAULT_SSID) - 1);
    else if (json)
        ESP_LOGE(REST_TAG, "Invalid SSID length (1-31)");

    json = cJSON_GetObjectItem(root, "wifi_pass");
    if (json && strlen(json->valuestring) < 64 && strlen(json->valuestring) > 7)
        strncpy((char *) DEFAULT_PWD, json->valuestring, sizeof(DEFAULT_PWD) - 1);
    else if (json)
        ESP_LOGE(REST_TAG, "Invalid password length (8-63)");


    json = cJSON_GetObjectItem(root, "ap_channel");
    if (json && json->valueint > 0 && json->valueint < 14) {
        DEFAULT_CHANNEL = json->valueint;
    } else if (json) {
        ESP_LOGE(REST_TAG, "No a valid wifi channel (1-13). Not changing!");
    }

    json = cJSON_GetObjectItem(root, "trans_pack_size");
    if (json) TRANSPARENT_BUF_SIZE = json->valueint;
    json = cJSON_GetObjectItem(root, "tx_pin");
    if (json) DB_UART_PIN_TX = json->valueint;
    json = cJSON_GetObjectItem(root, "rx_pin");
    if (json) DB_UART_PIN_RX = json->valueint;
    json = cJSON_GetObjectItem(root, "baud");
    if (json) DB_UART_BAUD_RATE = json->valueint;

    json = cJSON_GetObjectItem(root, "telem_proto");
    if (json && (json->valueint == 1 || json->valueint == 4)) {
        SERIAL_PROTOCOL = json->valueint;
    } else if (json) {
        ESP_LOGW(REST_TAG, "telem_proto is not 1 (LTM/MSP) or 4 (MAVLink/Transparent). Changing to transparent");
        SERIAL_PROTOCOL = 4;
    }

    json = cJSON_GetObjectItem(root, "ltm_pp");
    if (json) LTM_FRAME_NUM_BUFFER = json->valueint;

    json = cJSON_GetObjectItem(root, "msp_ltm_port");
    if (json && (json->valueint == 0 || json->valueint == 1))
        MSP_LTM_SAMEPORT = json->valueint;
    else if (json)
        ESP_LOGE(REST_TAG, "Only option 0 or 1 are allowed for msp_ltm_port parameter! Not changing!");

    json = cJSON_GetObjectItem(root, "ap_ip");
    if (json && is_valid_ip4(json->valuestring)) {
        strncpy(DEFAULT_AP_IP, json->valuestring, sizeof(DEFAULT_AP_IP) - 1);
    } else if (json) {
        ESP_LOGE(REST_TAG, "New IP \"%s\" is not a valid IP address! Not changing!", json->valuestring);
    }

    json = cJSON_GetObjectItem(root, "inet_server_ip");
    if (json && is_valid_ip4(json->valuestring)) {
        strncpy(INET_SERVER_IP, json->valuestring, sizeof(INET_SERVER_IP) - 1);
    } else if (json) {
        ESP_LOGE(REST_TAG, "New Internet Server IP \"%s\" is not a valid IP address! Not changing!", json->valuestring);
    }

    json = cJSON_GetObjectItem(root, "inet_srv_port");
    if (json) INET_SERVER_PORT = json->valueint;

    json = cJSON_GetObjectItem(root, "inet_apn");
    if (json) strncpy(MODEM_PPP_APN, json->valuestring, sizeof(MODEM_PPP_APN) - 1);

    write_settings_to_nvs();
    ESP_LOGI(REST_TAG, "Settings changed!");
    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\n"
                            "    \"status\": \"success\",\n"
                            "    \"msg\": \"Settings changed!\"\n"
                            "  }");
    return ESP_OK;
}

/* Simple handler for getting system handler */
static esp_err_t system_info_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON_AddStringToObject(root, "idf_version", IDF_VER);
    cJSON_AddNumberToObject(root, "db_build_version", BUILDVERSION);
    cJSON_AddNumberToObject(root, "major_version", MAJOR_VERSION);
    cJSON_AddNumberToObject(root, "minor_version", MINOR_VERSION);
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *) sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

/* Simple handler for getting system handler */
static esp_err_t system_stats_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "read_bytes", uart_byte_count);
    cJSON_AddNumberToObject(root, "tcp_connected", num_connected_tcp_clients);
    cJSON_AddNumberToObject(root, "udp_connected", num_connected_udp_clients);
    int ber;
    int rssi;
    esp_modem_get_signal_quality(dce,&rssi,&ber);
    cJSON_AddNumberToObject(root, "signal_strength", 100 * (rssi / 31));
    char operator_name[50];
    int act;
    esp_modem_get_operator_name(dce,operator_name,&act);
    cJSON_AddStringToObject(root, "operator_name", operator_name);
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *) sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t system_reboot_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "msg", "Rebooting!");
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *) sys_info);
    cJSON_Delete(root);
    esp_restart();
    return ESP_OK;
}

/* Simple handler for getting data */
static esp_err_t settings_data_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid", (char *) DEFAULT_SSID);
    cJSON_AddStringToObject(root, "wifi_pass", (char *) DEFAULT_PWD);
    cJSON_AddNumberToObject(root, "ap_channel", DEFAULT_CHANNEL);
    cJSON_AddNumberToObject(root, "trans_pack_size", TRANSPARENT_BUF_SIZE);
    cJSON_AddNumberToObject(root, "tx_pin", DB_UART_PIN_TX);
    cJSON_AddNumberToObject(root, "rx_pin", DB_UART_PIN_RX);
    cJSON_AddNumberToObject(root, "baud", DB_UART_BAUD_RATE);
    cJSON_AddNumberToObject(root, "telem_proto", SERIAL_PROTOCOL);
    cJSON_AddNumberToObject(root, "ltm_pp", LTM_FRAME_NUM_BUFFER);
    cJSON_AddNumberToObject(root, "msp_ltm_port", MSP_LTM_SAMEPORT);
    cJSON_AddStringToObject(root, "ap_ip", DEFAULT_AP_IP);
    cJSON_AddStringToObject(root, "inet_apn", MODEM_PPP_APN);
    cJSON_AddStringToObject(root, "inet_server_ip", INET_SERVER_IP);
    cJSON_AddNumberToObject(root, "inet_srv_port", INET_SERVER_PORT);
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *) sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t start_rest_server(const char *base_path) {
    REST_CHECK(base_path, "wrong base path", err);
    rest_server_context_t *rest_context = calloc(1, sizeof(rest_server_context_t));
    REST_CHECK(rest_context, "No memory for rest context", err);
    strlcpy(rest_context->base_path, base_path, sizeof(rest_context->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(REST_TAG, "Starting HTTP Server");
    REST_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);

    /* URI handler for fetching system info */
    httpd_uri_t system_info_get_uri = {
            .uri = "/api/system/info",
            .method = HTTP_GET,
            .handler = system_info_get_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_info_get_uri);

    /* URI handler for fetching system info */
    httpd_uri_t system_stats_get_uri = {
            .uri = "/api/system/stats",
            .method = HTTP_GET,
            .handler = system_stats_get_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_stats_get_uri);

    /* URI handler for triggering system reboot */
    httpd_uri_t system_reboot_get_uri = {
            .uri = "/api/system/reboot",
            .method = HTTP_GET,
            .handler = system_reboot_get_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_reboot_get_uri);

    /* URI handler for fetching settings data */
    httpd_uri_t temperature_data_get_uri = {
            .uri = "/api/settings/request",
            .method = HTTP_GET,
            .handler = settings_data_get_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &temperature_data_get_uri);

    /* URI handler for light brightness control */
    httpd_uri_t settings_change_post_uri = {
            .uri = "/api/settings/change",
            .method = HTTP_POST,
            .handler = settings_change_post_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &settings_change_post_uri);

    /* URI handler for getting web server files */
    httpd_uri_t common_get_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = rest_common_get_handler,
            .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &common_get_uri);

    return ESP_OK;
    err_start:
    free(rest_context);
    err:
    return ESP_FAIL;
}
