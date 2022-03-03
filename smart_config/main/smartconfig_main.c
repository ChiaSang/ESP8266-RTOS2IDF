/* Esptouch example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"
#include "esp_smartconfig.h"
#include "smartconfig_ack.h"

#define ESP_SMARTCOFNIG_TYPE SC_TYPE_ESPTOUCH_AIRKISS

#define SMARTCONFIG_NAMESPACE "nvs_smartconfig"
#define SC_SSID_USER "SC_SSID_USER"
#define SC_PASSWD_USER "SC_PASSWD_USER"

static const char *TAG = "nvs_smartcopnfig";

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
static const int CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;

/*********************************************************************
 * Function Definition
 */
static void smartconfig_example_task(void *parm);
void connect_wifi_ap(uint8_t *ssid, uint8_t *password);

/**
 * @brief 初始化 NVS，来记录 SSID和密码
 */
static void nvs_smartconfig_init(void)
{
    // 初始化默认的NVS分区。
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // NVS分区已被截断，需要删除
        // 重试 nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/**
 * @brief 从NVS中获得 SSID和密码
 */
esp_err_t nvs_get_ssid_passwd(uint8_t *ssid, uint8_t *password)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // 以读模式，打开命令空间为 SMARTCONFIG_NAMESPACE 的非易失性存储。返回本次操作句柄 my_handle。
    err = nvs_open(SMARTCONFIG_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK)
        return err;

    // 读取 SSID 和密码
    size_t length = 33;
    err = nvs_get_str(my_handle, SC_SSID_USER, (char *)ssid, &length);
    length = 65;
    err = nvs_get_str(my_handle, SC_PASSWD_USER, (char *)password, &length);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        return err;
    ESP_LOGI(TAG, "NVS SSID = %s", ssid);
    ESP_LOGI(TAG, "NVS PASSWD = %s", password);

    // 关闭NVS
    nvs_close(my_handle);
    return ESP_OK;
}

/**
 * @brief 向NVS中写入保存 SSID和密码
 */
esp_err_t nvs_save_ssid_passwd(uint8_t *ssid, uint8_t *password)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // 以读写模式，打开命令空间为 storage 的非易失性存储。返回本次操作句柄 my_handle。
    err = nvs_open(SMARTCONFIG_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK)
        return err;

    // 写入 SSID 和密码
    err = nvs_set_str(my_handle, SC_SSID_USER, (char *)ssid);
    err = nvs_set_str(my_handle, SC_PASSWD_USER, (char *)password);
    // nvs_set_i32(my_handle, RESTART_CONTER_KEY, restart_counter);
    if (err != ESP_OK)
        return err;

    // 关闭NVS前，必须调用nvs_commit（）以确保将更改写入闪存
    err = nvs_commit(my_handle);
    if (err != ESP_OK)
        return err;

    // 关闭NVS
    nvs_close(my_handle);
    return ESP_OK;
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    static uint8_t ssid[33] = {0};
    static uint8_t password[65] = {0};
    static uint8_t failBit = 0; // 密码错误计数。0-无失败情况；++计数。在多次失败后，则判断为SSID和密码错误，会自动切换到 SmartConfig配网，而无需额外的按键辅助配网。
    static uint8_t apBit = 0;   // 成功连接WIFI热点标志位。连接过WIFI热点后，如再出现WIFI断连，则不再计数failBit，以免误切换为配网。

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "read wifi info from nvs");
        nvs_get_ssid_passwd(ssid, password);
        if (ssid[0] != 0x00)
        { // 如NVS中的SSID和密码可用，则直接用NVS中的信息建立WIFI连接
            ESP_LOGI(TAG, "get ssid | password from nvs");
            connect_wifi_ap(ssid, password);
        }
        else
        { // 如NVS没有配置，则启动 Smartconfig配网
            ESP_LOGI(TAG, "get ssid | password from smartconfig");
            xTaskCreate(smartconfig_example_task, "smartconfig_example_task", 4096, NULL, 3, NULL);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG, "wifi disconnected try to reconnect in 3 times");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
        if (apBit == 0)
        { // 在设备开机后，成功连接到WIFI热点前，如自动连接次数超过 RETYR_SSID_NUM，则判断为SSID和密码错误，会自动切换为 SmartConfig配网。
            ++failBit;
            ESP_LOGI(TAG, "fail num = %d", failBit);
            if (failBit >= 3)
            { // 连接超时，判断为SSID和密码错误，自动将连接方式切换为 SmartConfig配网
                ESP_LOGI(TAG, "nvs config fail! then get ssid | password from smartconfig");
                xTaskCreate(smartconfig_example_task, "smartconfig_example_task", 4096, NULL, 3, NULL);
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ESP_LOGI(TAG, "wifi connected");
        xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
        nvs_save_ssid_passwd(ssid, password);
        failBit = 0; // 清空错误计数
        apBit = 1;   // 将成功连接热点标志位置1，如再出现WIFI断连，则不再计数failBit，以免误切换为配网。
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE)
    {
        ESP_LOGI(TAG, "Scan done");
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL)
    {
        ESP_LOGI(TAG, "Found channel");
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD)
    {
        ESP_LOGI(TAG, "Got SSID and password");

        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
        wifi_config_t wifi_config;
        // uint8_t ssid[33] = {0};
        // uint8_t password[65] = {0};
        uint8_t rvd_data[33] = {0};

        bzero(&wifi_config, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
        wifi_config.sta.bssid_set = evt->bssid_set;

        if (wifi_config.sta.bssid_set == true)
        {
            memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
        }

        memcpy(ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(password, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SSID:%s", ssid);
        ESP_LOGI(TAG, "PASSWORD:%s", password);
        if (evt->type == SC_TYPE_ESPTOUCH_V2)
        {
            ESP_ERROR_CHECK(esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data)));
            ESP_LOGI(TAG, "RVD_DATA:%s", rvd_data);
        }

        ESP_ERROR_CHECK(esp_wifi_disconnect());
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE)
    {
        ESP_LOGI(TAG, "success");
        xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
    }
}

/**
 * @brief 按照输入的SSID和密码，连接指定WIFI热点
 */
void connect_wifi_ap(uint8_t *ssid, uint8_t *password)
{
    wifi_config_t wifi_config;

    // 按照输入的SSID和密码，配置WIFI
    bzero(&wifi_config, sizeof(wifi_config_t));
    memcpy(wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.bssid_set = 0;

    // ESP_LOGI 调试输出函数输入的 SSID 和密码
    ESP_LOGI(TAG, "SSID:%s", ssid);
    ESP_LOGI(TAG, "PASSWORD:%s", password);

    // 按照输入的SSID和密码，连接指定的AP
    ESP_ERROR_CHECK(esp_wifi_disconnect());
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_wifi_connect();
}

static void smartconfig_example_task(void *parm)
{
    EventBits_t uxBits;
    ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_AIRKISS));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));

    while (1)
    {
        uxBits = xEventGroupWaitBits(s_wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false, portMAX_DELAY);

        if (uxBits & CONNECTED_BIT)
        {
            ESP_LOGI(TAG, "WiFi Connected to ap");
        }

        if (uxBits & ESPTOUCH_DONE_BIT)
        {
            ESP_LOGI(TAG, "smartconfig over");
            esp_smartconfig_stop();
            vTaskDelete(NULL);
        }
    }
}

static void initialise_wifi(void)
{
    nvs_smartconfig_init();
    tcpip_adapter_init();
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main()
{
    ESP_ERROR_CHECK(nvs_flash_init());
    initialise_wifi();
}
