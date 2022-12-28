#include <stdio.h>
#include <esp_event.h>
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <esp_system.h>
#include <string.h>
#include <stdlib.h>
#include "esp_event.h"
#include <lwip/netif.h>
#include "esp_wifi.h"
#include "nvs.h"
#include <esp_log.h>
#include "nvs_flash.h"
#include "esp_now.h"
#include <driver/adc.h>
#include "time.h"

#define TAG "lr_connect"

enum station_type {
    MASTER = 0,
    SLAVE
};

enum States {
    ST_IDLE = 0,
    ST_SCAN,
    ST_CONNECT
};

enum States curState = ST_SCAN;

enum station_type type = MASTER; // Choose SLAVE or MASTER

typedef struct lr_wifi_client {
    esp_netif_t *esp_netif;					///< Initialized networking interface.
    EventGroupHandle_t status_bits;         ///< Event group for BIT management.
    char hostname[30];
} lr_wifi_client_t;

lr_wifi_client_t *wifi_client = NULL;

static TaskHandle_t htask;

wifi_scan_config_t scan_conf;

wifi_ap_record_t *global_scans = NULL;

wifi_config_t *next_wifi = NULL;

uint16_t global_scans_num = 32;

uint8_t global_scans_count = 0;

esp_now_peer_info_t peerInfo;

bool already_peered = false;

esp_timer_handle_t periodic_timer;

const int WIFI_SCAN_FINISHED_BIT = BIT0;
const int WIFI_CONNECTED = BIT1;
const int WIFI_DISCONNECT_BIT = BIT2;


typedef struct messageStruct {
    uint8_t hall_sensor;
}messageStruct_t;

messageStruct_t message;

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_STOP");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "WIFI_EVENT_SCAN_DONE");
        xEventGroupSetBits(wifi_client->status_bits, WIFI_SCAN_FINISHED_BIT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED");
        wifi_event_sta_disconnected_t *dr = event_data;
        wifi_err_reason_t reason = dr->reason;
        ESP_LOGW(TAG, "Disconnected. Reason: %d", reason);
        if (reason != WIFI_REASON_ASSOC_LEAVE) {
            xEventGroupSetBits(wifi_client->status_bits, WIFI_DISCONNECT_BIT);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED");
        xEventGroupSetBits(wifi_client->status_bits, WIFI_CONNECTED);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP");
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:%s", ip4addr_ntoa((const ip4_addr_t *)&event->ip_info.ip));
    }
}


void onDataSent(const uint8_t *macAddress, esp_now_send_status_t sendStatus)
{
    if (sendStatus == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGI(TAG,"sent succes");
    }
    else {
        ESP_LOGE(TAG,"ERROR: sent failed!");
    }
}


void onDataReceive(const uint8_t *macAddress, const uint8_t *incomingData, int dataLength)
{
    memcpy(&message, incomingData, sizeof(message));
    if (!(message.hall_sensor > 50 && message.hall_sensor < 75)) {
        gpio_set_level(GPIO_NUM_2, 1);
    }
    else {
        gpio_set_level(GPIO_NUM_2, 0);
    }

    ESP_LOGI(TAG,"Hallsensor: %d", message.hall_sensor);
}


static void wifi_master_task(void *pvParameters) {
    EventBits_t uxBits;
    if ((global_scans = calloc(32, sizeof(wifi_ap_record_t))) == NULL) {
        ESP_LOGE(TAG, "Error no more free Memory");
        vTaskDelete(NULL);
    }
    if ((next_wifi = calloc(1, sizeof(wifi_config_t))) == NULL) {
        ESP_LOGE(TAG, "Error no more free Memory");
        vTaskDelete(NULL);
    }
    while (true) {
        switch (curState) {
            case ST_IDLE:
                uxBits = xEventGroupWaitBits(wifi_client->status_bits, WIFI_DISCONNECT_BIT, pdTRUE, pdFALSE,
                                             pdMS_TO_TICKS(100));
                if ((uxBits & WIFI_DISCONNECT_BIT) != 0) {
                    ESP_LOGI(TAG, "Connection got lost perform scan again...");
                    curState = ST_SCAN;
                    break;
                }
                else {
                    message.hall_sensor = hall_sensor_read();
                    ESP_LOGI(TAG, "Send packet");
                    esp_err_t result = esp_now_send(peerInfo.peer_addr, (uint8_t *)&message, sizeof(message));
                    if (result != ESP_OK) {
                        ESP_LOGE(TAG,"Sending error");
                    }
                }

                break;
            case ST_SCAN:
                xEventGroupClearBits(wifi_client->status_bits, WIFI_SCAN_FINISHED_BIT);
                if (esp_wifi_scan_start(&scan_conf, false) != ESP_OK) {
                    ESP_LOGE(TAG, "Error at wifi_scan_start");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                uxBits = xEventGroupWaitBits(wifi_client->status_bits, WIFI_SCAN_FINISHED_BIT, pdTRUE, pdFALSE,
                                             pdMS_TO_TICKS(3000));
                if ((uxBits & WIFI_SCAN_FINISHED_BIT) != 0) {
                    esp_wifi_scan_get_ap_records(&global_scans_num, global_scans);
                    global_scans_count = global_scans_num;
                    global_scans_num = 32;
                    ESP_LOGI(TAG, "Found %d Networks during scan", global_scans_count);
                    for (uint8_t i = 0; i < global_scans_count; i++) {
                        ESP_LOGI(TAG, "%d. Scan: %s (%d)", i + 1, global_scans[i].ssid, global_scans[i].rssi);
                        if (strcmp((const char *) global_scans[i].ssid, "Slave_1") == 0) {
                            ESP_LOGI(TAG, "Found Slave");
                            memcpy(next_wifi->sta.ssid, global_scans[i].ssid, sizeof(global_scans[i].ssid));
                            memcpy(next_wifi->sta.password, "guest123456", sizeof("guest123456"));
                            memcpy(next_wifi->sta.bssid, global_scans[i].bssid, sizeof(global_scans[i].bssid));
                            curState = ST_CONNECT;
                            break;
                        }
                    }
                } else {
                    ESP_LOGE(TAG, "Error in scan");
                }
                break;
            case ST_CONNECT:
                esp_wifi_set_config(WIFI_IF_STA,next_wifi);
                xEventGroupClearBits(wifi_client->status_bits, WIFI_SCAN_FINISHED_BIT);
                esp_wifi_connect();
                uxBits = xEventGroupWaitBits(wifi_client->status_bits, WIFI_SCAN_FINISHED_BIT | WIFI_DISCONNECT_BIT, pdTRUE, pdFALSE,
                                             pdMS_TO_TICKS(3000));
                if ((uxBits & WIFI_CONNECTED) != 0) {
                    ESP_LOGI(TAG, "Connected! go to Idle...");
                    if (!already_peered) {
                        memcpy(peerInfo.peer_addr, next_wifi->sta.bssid, sizeof(next_wifi->sta.bssid));
                        peerInfo.channel = next_wifi->sta.channel;
                        peerInfo.encrypt = false;
                        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
                            ESP_LOGE(TAG, "couldnt peer with slave!");
                        } else {
                            ESP_LOGI(TAG, "Succesfully paired with Slave");
                            already_peered = true;
                        }
                    }
                    curState = ST_IDLE;
                    break;
                }
                else {
                    ESP_LOGE(TAG, "NOT connected go to Scan!");
                    curState = ST_SCAN;
                }
                break;
            default:
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


void start_master() {
    esp_wifi_start();
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    xTaskCreatePinnedToCore(wifi_master_task, "wifi_master_task", 4096, NULL, 5, htask,  PRO_CPU_NUM);
}

void init_master() {
    wifi_client = calloc(1, sizeof(lr_wifi_client_t));
    if (wifi_client == NULL) {
        ESP_LOGE(TAG, "Error no Memory");
        abort();
    }
    nvs_flash_init();
    wifi_client->status_bits = xEventGroupCreate();
    wifi_client->esp_netif = esp_netif_create_default_wifi_sta();
    sprintf(wifi_client->hostname, "HTOOL-TEST");
    if (esp_netif_set_hostname(wifi_client->esp_netif, wifi_client->hostname) != ESP_OK) {
        ESP_LOGE(TAG, "Error general");
        abort();
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    wifi_country_t ccconf = {
            .cc = "00",
            .schan = 1,
            .nchan = 13,
            .policy = WIFI_COUNTRY_POLICY_MANUAL
    };

    if (esp_wifi_set_country(&ccconf) != ESP_OK) {
        ESP_LOGE(TAG, "Error during setup of wifi country code!");
    }

    scan_conf.show_hidden = true;
    scan_conf.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_conf.scan_time.active.min = 100;
    scan_conf.scan_time.active.max = 150;
    scan_conf.channel = 1;

    ESP_ERROR_CHECK(esp_now_init());
    esp_now_register_send_cb(onDataSent);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
}


static void wifi_slave_task(void *pvParameters) {
    while (true) {
        ESP_LOGI(TAG, "Slave running");
        vTaskDelay(10000);
    }
}

void start_slave() {
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    esp_now_register_recv_cb(onDataReceive);
    xTaskCreatePinnedToCore(wifi_slave_task, "wifi_slave_task", 4096, NULL, 5, htask,  PRO_CPU_NUM);
}

void init_slave() {
    wifi_client = calloc(1, sizeof(lr_wifi_client_t));
    if (wifi_client == NULL) {
        ESP_LOGE(TAG, "Error no Memory");
        abort();
    }
    nvs_flash_init();
    wifi_client->status_bits = xEventGroupCreate();
    wifi_client->esp_netif = esp_netif_create_default_wifi_sta();
    sprintf(wifi_client->hostname, "lr_esp32");
    if (esp_netif_set_hostname(wifi_client->esp_netif, wifi_client->hostname) != ESP_OK) {
        ESP_LOGE(TAG, "Error general");
        abort();
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_LR));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    wifi_country_t ccconf = {
            .cc = "00", // worldwide setting, for regional use .cc = "AT"
            .schan = 1,
            .nchan = 13,
            .policy = WIFI_COUNTRY_POLICY_MANUAL
    };

    if (esp_wifi_set_country(&ccconf) != ESP_OK) {
        ESP_LOGE(TAG, "Error during setup of wifi country code!");
    }

    wifi_config_t config = {
            .ap = {
                    .ssid = "Slave_1",
                    .ssid_len = strlen("Slave_1"),
                    .channel = 1,
                    .password = "guest123456",
                    .max_connection = 1,
                    .authmode = WIFI_AUTH_WPA_WPA2_PSK,
                    .beacon_interval = 100
            },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
}

void init_station(enum station_type type) {
    switch (type) {
        case MASTER:
            init_master();
            adc1_config_width(ADC_WIDTH_BIT_12);
            start_master();
            break;
        case SLAVE:
            init_slave();
            gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
            start_slave();
            break;
        default:
            break;
    }
}

static void initialize_esp_modules() {
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
}

void app_main(void) {
    printf("Start Long Range Connect\r\n");

    esp_log_level_set("*",ESP_LOG_INFO);

    initialize_esp_modules();

    init_station(type);

    printf("Long Range Connect started completed\r\n");
}
