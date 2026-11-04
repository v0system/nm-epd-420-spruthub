// Скопируй в secrets.h и заполни. Без secrets.h прошивка соберётся с пустыми
// значениями и при старте поднимет SoftAP nm-epd-setup для настройки.
#pragma once

#define DEF_WIFI_SSID ""
#define DEF_WIFI_PASS ""

#define DEF_MQTT_HOST ""
#define DEF_MQTT_PORT 1883
#define DEF_MQTT_USER ""
#define DEF_MQTT_PASS ""
#define DEF_MQTT_PREFIX ""

// подпись справа в футере после IP (необязательно)
#define DEF_FOOTER_TAG ""
