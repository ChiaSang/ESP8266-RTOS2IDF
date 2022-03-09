#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi_smartconfig.h"
#include "sntp_systime.h"
#include "app_mqtt_code.h"

static const char *TAG = "app";

void app_main()
{
    // nvs_flash_erase();
    initialise_wifi_smartconfig();
    initial_sntp();

    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset Reason : %X", reason);

    app_start();
}
