#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "mqtt_client.h"
#include "app_mqtt_code.h"
#include "driver/uart.h"
#include "cJSON.h"
#include "math.h"

static char *TAG = "onenet";
static oneNET_connect_msg_t *oneNET_connect_msg_static;
extern const uint8_t client_cert_pem_start[] asm("_binary_MQTTS_certificate_pem_start");
extern const uint8_t client_cert_pem_end[] asm("_binary_MQTTS_certificate_pem_end");

#define EX_UART_NUM UART_NUM_0
#define BUF_SIZE (2048)
#define RD_BUF_SIZE (BUF_SIZE)

bool bit_sub_post;  // post主题订阅成功标志位。0-未订阅；1-订阅成功。
bool bit_sub_event; // set主题订阅成功标志位。0-未订阅；1-订阅成功。
bool bit_sub_get;   // set主题订阅成功标志位。0-未订阅；1-订阅成功。

static QueueHandle_t uart0_queue;

oneNET_connect_msg_t oneNET_connect_msg;

sensor_7in1_t s7in1;

void parse_7in1_str(sensor_7in1_t *s7in1, uint8_t *arr, int len)
{
    if (sizeof(s7in1))
    {
        s7in1->CO2 = arr[2] * 256 + arr[3];
        s7in1->CH2O = arr[4] * 256 + arr[5];
        s7in1->TVOC = arr[6] * 256 + arr[7];
        s7in1->PM25 = arr[8] * 256 + arr[9];
        s7in1->PM10 = arr[10] * 256 + arr[11];
        s7in1->Temp = arr[12] + arr[13] * 0.1;
        s7in1->Humi = arr[14] + arr[15] * 0.1;
    }
    else
    {
        s7in1->CH2O = 0;
        s7in1->CO2 = 0;
        s7in1->TVOC = 0;
        s7in1->PM10 = 0;
        s7in1->PM25 = 0;
        s7in1->Temp = 0.0;
        s7in1->Humi = 0.0;
    }

    // ESP_LOGI(TAG, "CO2: %d CH2O : %d TVOC : %d PM25 : %d PM10 : %d Temp : %.2f Humi : %.2f", s7in1->CO2, s7in1->CH2O, s7in1->TVOC, s7in1->PM25, s7in1->PM10, s7in1->Temp, s7in1->Humi);
}

char *packet_json(sensor_7in1_t *s7in1)
{
    ESP_LOGI(TAG, "CO2: %d CH2O : %d TVOC : %d PM25 : %d PM10 : %d Temp : %.2f Humi : %.2f", s7in1->CO2, s7in1->CH2O, s7in1->TVOC, s7in1->PM25, s7in1->PM10, s7in1->Temp, s7in1->Humi);
    char packet_id[18] = {0};
    time_t now;
    time(&now);
    sprintf(packet_id, "%ld", now);

    cJSON *pRoot = cJSON_CreateObject();
    cJSON_AddStringToObject(pRoot, "id", packet_id);
    cJSON_AddStringToObject(pRoot, "version", "1.0");

    cJSON *pParams = cJSON_CreateObject();
    cJSON_AddItemToObject(pRoot, "params", pParams);

    cJSON *pCO2 = cJSON_CreateObject();
    cJSON_AddItemToObject(pParams, "CO2", pCO2);
    cJSON_AddNumberToObject(pCO2, "value", s7in1->CO2);

    cJSON *pCH2O = cJSON_CreateObject();
    cJSON_AddItemToObject(pParams, "CH2O", pCH2O);
    cJSON_AddNumberToObject(pCH2O, "value", s7in1->CH2O);

    cJSON *pTVOC = cJSON_CreateObject();
    cJSON_AddItemToObject(pParams, "TVOC", pTVOC);
    cJSON_AddNumberToObject(pTVOC, "value", s7in1->TVOC);

    cJSON *pPM25 = cJSON_CreateObject();
    cJSON_AddItemToObject(pParams, "PM25", pPM25);
    cJSON_AddNumberToObject(pPM25, "value", s7in1->PM25);

    cJSON *pPM10 = cJSON_CreateObject();
    cJSON_AddItemToObject(pParams, "PM10", pPM10);
    cJSON_AddNumberToObject(pPM10, "value", s7in1->PM10);

    cJSON *pTemp = cJSON_CreateObject();
    cJSON_AddItemToObject(pParams, "Temperature", pTemp);
    cJSON_AddNumberToObject(pTemp, "value", s7in1->Temp);

    cJSON *pRH = cJSON_CreateObject();
    cJSON_AddItemToObject(pParams, "RelativeHumidity", pRH);
    cJSON_AddNumberToObject(pRH, "value", s7in1->Humi);

    char *sendData = cJSON_PrintUnformatted(pRoot);
    memset(packet_id, 0, sizeof(packet_id));
    // ESP_LOGI(TAG, "up: --> %s", sendData);
    cJSON_Delete(pRoot); // 释放cJSON_CreateObject ()分配出来的内存空间
    return sendData;
}

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t *dtmp = (uint8_t *)malloc(RD_BUF_SIZE);

    for (;;)
    {
        // Waiting for UART event.
        if (xQueueReceive(uart0_queue, (void *)&event, (portTickType)portMAX_DELAY))
        {
            bzero(dtmp, RD_BUF_SIZE);

            switch (event.type)
            {
            // Event of UART receving data
            // We'd better handler data event fast, there would be much more data events than
            // other types of events. If we take too much time on data event, the queue might be full.
            case UART_DATA:
                ESP_LOGI(TAG, "[UART DATA]: %d", event.size);
                int len = uart_read_bytes(EX_UART_NUM, dtmp, event.size, portMAX_DELAY);
                // 打印原始16进制数据
                int i = 0;
                dtmp[len] = 0;
                char sensor_hex_str[64] = "";
                while (i < len)
                {
                    char hex_data[4];
                    sprintf(hex_data, "%02X", dtmp[i]);
                    strcat(sensor_hex_str, hex_data);
                    i += 1;
                }
                ESP_LOGI(TAG, "initial data: %s", sensor_hex_str);
                parse_7in1_str(&s7in1, dtmp, len);
                break;

            // Event of HW FIFO overflow detected
            case UART_FIFO_OVF:
                ESP_LOGI(TAG, "hw fifo overflow");
                // If fifo overflow happened, you should consider adding flow control for your application.
                // The ISR has already reset the rx FIFO,
                // As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(EX_UART_NUM);
                xQueueReset(uart0_queue);
                break;

            // Event of UART ring buffer full
            case UART_BUFFER_FULL:
                ESP_LOGI(TAG, "ring buffer full");
                // If buffer full happened, you should consider encreasing your buffer size
                // As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(EX_UART_NUM);
                xQueueReset(uart0_queue);
                break;

            case UART_PARITY_ERR:
                ESP_LOGI(TAG, "uart parity error");
                break;

            // Event of UART frame error
            case UART_FRAME_ERR:
                ESP_LOGI(TAG, "uart frame error");
                break;

            // Others
            default:
                ESP_LOGI(TAG, "uart event type: %d", event.type);
                break;
            }
        }
    }

    free(dtmp);
    dtmp = NULL;
    vTaskDelete(NULL);
}

void initUart(void)
{
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};
    uart_param_config(UART_NUM_0, &uart_config);

    // Install UART driver, and get the queue.
    uart_driver_install(UART_NUM_0, BUF_SIZE * 2, BUF_SIZE * 2, 100, &uart0_queue, 0);
}

/**
 * @brief oneNET_publish
 *          定时上传属性任务
 * @param arg
 * @return void*
 */
void oneNET_publish(esp_mqtt_client_handle_t client, int period)
{
    char topic[128] = {0};
    char *device_property;
    sprintf(topic, "$sys/%s/%s/thing/property/post", oneNET_connect_msg_static->produt_id, oneNET_connect_msg_static->device_name);
    while (1)
    {
        device_property = packet_json(&s7in1);
        ESP_LOGI(TAG, "up: --> %s", device_property);
        esp_mqtt_client_publish(client, topic, device_property, strlen(device_property), 1, 0);
        memset(device_property, 0, sizeof(*device_property));
        vTaskDelay(period / portTICK_PERIOD_MS);
    }
}

/**
 * @brief mqtt 事件处理函数
 *
 * @param event
 * @return esp_err_t
 */
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;

    static int post_sub_id = 0;  // 订阅post主题的消息ID
    static int get_sub_id = 0;   // 订阅set主题的消息ID
    static int event_sub_id = 0; // 订阅event主题的消息ID

    // your_context_t *context = event->context;
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        char dev_property_topic[128] = {0};
        //订阅 设备属性上报响应
        memset(dev_property_topic, 0, 128);
        sprintf(dev_property_topic, "$sys/%s/%s/thing/property/post/reply", oneNET_connect_msg_static->produt_id, oneNET_connect_msg_static->device_name);
        post_sub_id = esp_mqtt_client_subscribe(client, dev_property_topic, 1);
        //设备属性设置Topic
        memset(dev_property_topic, 0, 128);
        sprintf(dev_property_topic, "$sys/%s/%s/thing/property/set", oneNET_connect_msg_static->produt_id, oneNET_connect_msg_static->device_name);
        event_sub_id = esp_mqtt_client_subscribe(client, dev_property_topic, 1);
        //云平台主动获取属性 Topic
        memset(dev_property_topic, 0, 128);
        sprintf(dev_property_topic, "$sys/%s/%s/thing/property/get", oneNET_connect_msg_static->produt_id, oneNET_connect_msg_static->device_name);
        get_sub_id = esp_mqtt_client_subscribe(client, dev_property_topic, 1);
        //订阅OTA opic
        memset(dev_property_topic, 0, 128);
        sprintf(dev_property_topic, "ota/%s/%s/thing/property/get", oneNET_connect_msg_static->produt_id, oneNET_connect_msg_static->device_name);
        esp_mqtt_client_subscribe(client, dev_property_topic, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        if (event->msg_id == post_sub_id)
        {
            bit_sub_post = 1;
        }
        else if (event->msg_id == event_sub_id)
        {
            bit_sub_event = 1;
        }
        else if (event->msg_id == get_sub_id)
        {
            bit_sub_get = 1;
        }
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        ESP_LOGI(TAG, "down: <-- \"%.*s\", num = %d", event->data_len, event->data, event->data_len);
        ESP_LOGI(TAG, "topic: \"%.*s\"", event->topic_len, event->topic);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
    return ESP_OK;
}

void mqtt_event_handler(void *handler_pvParameterss, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

/**
 * @brief 启动连接 oneNET
 *
 * @return esp_err_t
 */
esp_err_t app_open_mqtt_connection(oneNET_connect_msg_t *oneNET_connect_msg)
{
    onenet_connect_msg_init(oneNET_connect_msg, ONENET_METHOD_MD5);
    oneNET_connect_msg_static = oneNET_connect_msg;
    esp_mqtt_client_config_t oneNET_client_cfg = {
        .host = ONENET_HOST,
        .port = ONENET_PORT,
        .client_id = oneNET_connect_msg->device_name,
        .username = oneNET_connect_msg->produt_id,
        .password = oneNET_connect_msg->token,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&oneNET_client_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);

    oneNET_publish(client, 5000);

    return ESP_OK;
}

void app_start(void)
{
    initUart();
    xTaskCreate(uart_event_task, "uart_event_task", 2048, NULL, 12, NULL);
    app_open_mqtt_connection(&oneNET_connect_msg);
}