#ifndef APP_MQTT_CODE_H
#define APP_MQTT_CODE_H
#include "initialToken.h"
// 218.201.45.7
#define ONENET_HOST "studio-mqtt.heclouds.com"
#define ONENET_PORT 1883

typedef struct S7IN1
{
    int CO2;
    int CH2O;
    int TVOC;
    int PM25;
    int PM10;
    float Temp;
    float Humi;
} sensor_7in1_t;

void app_start();

#endif
