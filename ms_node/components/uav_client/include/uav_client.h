#pragma once

#include "esp_err.h"

// Configuration
#define UAV_WIFI_SSID "WSN_AP"
#define UAV_WIFI_PASS "raspberry"
#define UAV_SERVER_PORT 8080
#define UAV_SECRET_KEY "pi_secret_key_12345"

// Dynamic URLs - will be constructed with discovered gateway IP
extern char g_uav_server_url_onboard[64];
extern char g_uav_server_url_data[64]; 
extern char g_uav_server_url_ack[64];

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

/**
 * @brief Cleanup UAV client resources
 * 
 * Call this after UAV onboarding completes (success or failure) and before
 * reinitializing ESP-NOW. Destroys the WiFi STA netif that was created.
 */
void uav_client_cleanup(void);
