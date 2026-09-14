#pragma once
#include <stdint.h>

const char wifiSSID[] = "";
const char wifiPass[] = "";
const char hotspotSSID[] = "";
const char hotspotPass[] = "";
const char qWeatherURL[] = "https://example.com/weather";
const char qWeatherKey[] = "";
const char qWeatherLocation[] = "";
const char classTimeAPI[] = "https://example.com/classes.csv";
// Public RFC 6238 SHA-1 test key; replace for actual authentication.
uint8_t hmacKey[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
                      '1', '2', '3', '4', '5', '6', '7', '8', '9', '0' };
// PEM root CA(s) for the configured HTTPS services.
const char apiRootCA[] = "";
