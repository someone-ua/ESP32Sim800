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

#ifndef DB_ESP32_GLOBALS_H
#define DB_ESP32_GLOBALS_H

#include <freertos/event_groups.h>
#include "esp_modem_api.h"

#define MAX_LTM_FRAMES_IN_BUFFER 5
#define BUILDVERSION 7
#define MAJOR_VERSION 1
#define MINOR_VERSION 0

#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"

// can be set by user
extern uint8_t DEFAULT_SSID[32];
extern uint8_t DEFAULT_PWD[64];
extern char DEFAULT_AP_IP[32];
extern char INET_SERVER_IP[32];
extern int INET_SERVER_PORT;
extern char MODEM_PPP_APN[32];
extern uint8_t DEFAULT_CHANNEL;
extern uint8_t SERIAL_PROTOCOL;  // 1=MSP, 3=MAVLink/transparent
extern uint8_t DB_UART_PIN_TX;
extern uint8_t DB_UART_PIN_RX;
extern int DB_UART_BAUD_RATE;
extern uint16_t TRANSPARENT_BUF_SIZE;
extern uint8_t LTM_FRAME_NUM_BUFFER;    // Number of LTM frames per UDP packet (min = 1; max = 5)
extern uint8_t MSP_LTM_SAMEPORT;        // 0 = no (1607 MSP, 1604 LTM); >0 = yes (1604)

extern uint32_t uart_byte_count;
extern int8_t num_connected_tcp_clients;
extern int8_t num_connected_udp_clients;
extern esp_modem_dce_t *dce;

#endif //DB_ESP32_GLOBALS_H
