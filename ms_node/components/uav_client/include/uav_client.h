#pragma once

#include "esp_err.h"

// Configuration
#define UAV_WIFI_SSID "WSN_AP"
#define UAV_WIFI_PASS "raspberry"
#define UAV_SERVER_URL_ONBOARD "http://10.42.0.1:8080/onboard"
#define UAV_SERVER_URL_DATA "http://10.42.0.1:8080/data"
#define UAV_SERVER_URL_ACK "http://10.42.0.1:8080/ack"
#define UAV_SECRET_KEY "pi_secret_key_12345"

/**
 * @brief Run the UAV Client Onboarding sequence
 *
 * 1. Connect to WSN_AP
 * 2. Generate Token
 * 3. POST /onboard
 * 4. Parse Session ID
 * 5. POST /ack
 * 6. Disconnect
 *
 * @return ESP_OK on success, failure otherwise
 */
esp_err_t uav_client_run_onboarding(void);
