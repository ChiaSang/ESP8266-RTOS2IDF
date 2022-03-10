# 7合1传感器

上传数据到OneNet

```

ESP_RST_UNKNOWN = 0,   Reset reason can not be determined
ESP_RST_POWERON,       Reset due to power-on event 电源复位
ESP_RST_EXT,           Reset by external pin (not applicable for ESP8266)
ESP_RST_SW,            Software reset via esp_restart 代码调用esp_restart()方法复位
ESP_RST_PANIC,         Software reset due to exception/panic 代码异常
ESP_RST_INT_WDT,       Reset (software or hardware) due to interrupt watchdog 软件或硬件中断异常导致看门狗
ESP_RST_TASK_WDT,      Reset due to task watchdog 任务超时导致看门狗复位
ESP_RST_WDT,           Reset due to other watchdogs 其他看门狗
ESP_RST_DEEPSLEEP,     Reset after exiting deep sleep mode 睡眠模式唤醒导致
ESP_RST_BROWNOUT,      Brownout reset (software or hardware)
ESP_RST_SDIO,          Reset over SDIO

```