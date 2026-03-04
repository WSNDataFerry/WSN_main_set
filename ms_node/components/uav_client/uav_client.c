#include "uav_client.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"
#include "mbedtls/md.h"
#include "storage_manager.h"
#include <string.h>
#include <sys/stat.h>

#define MAX_BUFFER_SIZE 65536  // Max 64KB buffer for UAV upload
#define MAX_HTTP_RESPONSE_SIZE 1024  // Max response body for /onboard, /ack

// Default gateway for Raspberry Pi hotspot (10.42.0.x network)
#define UAV_DEFAULT_GATEWAY "10.42.0.1"

// WiFi event group bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_GOT_IP_BIT    BIT2

// Max WiFi connect retries before giving up
#define WIFI_MAX_RETRIES   5

static const char *TAG = "UAV_CLIENT";

// WiFi event group for synchronization
static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_wifi_retry_count = 0;
static esp_event_handler_instance_t s_wifi_handler_instance = NULL;
static esp_event_handler_instance_t s_ip_handler_instance = NULL;

// --- HTTP Response Buffer (used by event handler) ---
typedef struct {
  char *buffer;
  int len;
  int max_len;
} http_response_t;

// HTTP event handler to capture response body during esp_http_client_perform()
static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
  http_response_t *resp = (http_response_t *)evt->user_data;
  switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
      if (resp && resp->buffer) {
        int copy_len = evt->data_len;
        if (resp->len + copy_len >= resp->max_len) {
          copy_len = resp->max_len - resp->len - 1;
        }
        if (copy_len > 0) {
          memcpy(resp->buffer + resp->len, evt->data, copy_len);
          resp->len += copy_len;
          resp->buffer[resp->len] = '\0';
        }
      }
      break;
    default:
      break;
  }
  return ESP_OK;
}

// Global URL variables (declared in header)
char g_uav_server_url_onboard[64];
char g_uav_server_url_data[64]; 
char g_uav_server_url_ack[64];

// Static netif handle for WiFi STA (created once, reused)
static esp_netif_t *s_sta_netif = NULL;

// Session counter for multi-onboarding tracking
static uint32_t s_onboard_session_count = 0;

// --- Gateway Discovery Helper (using esp_netif) ---
static esp_err_t discover_gateway_ip(char *gateway_ip, size_t ip_buf_len) {
  // Get IP info using esp_netif API (proper ESP-IDF way)
  if (s_sta_netif == NULL) {
    ESP_LOGE(TAG, "STA netif not created");
    // Fallback to known Raspberry Pi hotspot gateway
    strncpy(gateway_ip, UAV_DEFAULT_GATEWAY, ip_buf_len);
    ESP_LOGW(TAG, "Using fallback gateway IP: %s", gateway_ip);
    return ESP_OK;
  }

  esp_netif_ip_info_t ip_info;
  esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
  
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get IP info: %s", esp_err_to_name(err));
    strncpy(gateway_ip, UAV_DEFAULT_GATEWAY, ip_buf_len);
    ESP_LOGW(TAG, "Using fallback gateway IP: %s", gateway_ip);
    return ESP_OK;
  }

  if (ip_info.ip.addr == 0) {
    ESP_LOGE(TAG, "No IP address assigned yet");
    strncpy(gateway_ip, UAV_DEFAULT_GATEWAY, ip_buf_len);
    ESP_LOGW(TAG, "Using fallback gateway IP: %s", gateway_ip);
    return ESP_OK;
  }

  // Check if we have a valid gateway
  if (ip_info.gw.addr != 0) {
    snprintf(gateway_ip, ip_buf_len, IPSTR, IP2STR(&ip_info.gw));
    ESP_LOGI(TAG, "Discovered gateway IP: %s (our IP: " IPSTR ")", 
             gateway_ip, IP2STR(&ip_info.ip));
  } else {
    // Construct gateway from our IP (common pattern: .1)
    esp_ip4_addr_t gw;
    gw.addr = (ip_info.ip.addr & 0x00FFFFFF) | 0x01000000;  // Replace last octet with .1 (network byte order)
    snprintf(gateway_ip, ip_buf_len, IPSTR, IP2STR(&gw));
    ESP_LOGW(TAG, "Constructed gateway IP: %s (from our IP: " IPSTR ")", 
             gateway_ip, IP2STR(&ip_info.ip));
  }
  
  return ESP_OK;
}

// --- HMAC Helper ---
static void generate_token(const char *node_id, const char *metadata,
                           char *output_hex) {
  char payload[128];
  snprintf(payload, sizeof(payload), "%s|%s", node_id, metadata);

  const char *key = UAV_SECRET_KEY;
  unsigned char output[32];

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, strlen(key));
  mbedtls_md_hmac_update(&ctx, (const unsigned char *)payload, strlen(payload));
  mbedtls_md_hmac_finish(&ctx, output);
  mbedtls_md_free(&ctx);

  for (int i = 0; i < 32; i++) {
    sprintf(output_hex + (i * 2), "%02x", output[i]);
  }
  output_hex[64] = 0;
}

// --- Wi-Fi Event Handler ---
// Handles WiFi connection events using proper ESP-IDF event-driven pattern.
// Sets event group bits to unblock wifi_join() waiting.
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    // WiFi started - now it's safe to call esp_wifi_connect()
    ESP_LOGI(TAG, "WiFi STA started, initiating connection...");
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
    if (s_wifi_retry_count < WIFI_MAX_RETRIES) {
      s_wifi_retry_count++;
      ESP_LOGW(TAG, "WiFi disconnected (reason=%d), retrying %d/%d...", 
               disconn->reason, s_wifi_retry_count, WIFI_MAX_RETRIES);
      vTaskDelay(pdMS_TO_TICKS(500));  // Brief delay before retry
      esp_wifi_connect();
    } else {
      ESP_LOGE(TAG, "WiFi connection failed after %d retries (last reason=%d)", 
               WIFI_MAX_RETRIES, disconn->reason);
      if (s_wifi_event_group) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      }
    }
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
    ESP_LOGI(TAG, "WiFi connected to AP, waiting for IP...");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_wifi_retry_count = 0;
    if (s_wifi_event_group) {
      xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT);
    }
  }
}

// --- Wi-Fi Helper ---
// Creates STA netif if needed, connects to AP using event-driven approach,
// and discovers gateway IP.

static esp_err_t wifi_join(const char *ssid, const char *pass) {
  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, pass,
          sizeof(wifi_config.sta.password));

  ESP_LOGI(TAG, "Connecting to %s...", ssid);
  
  // Create event group for WiFi synchronization
  if (s_wifi_event_group == NULL) {
    s_wifi_event_group = xEventGroupCreate();
  }
  xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_GOT_IP_BIT);
  s_wifi_retry_count = 0;
  
  // Create the default WiFi STA netif if not already created
  if (s_sta_netif == NULL) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
      ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
      return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Created default WiFi STA netif");
  } else {
    // Netif is being reused from a previous session.
    // Restart DHCP client to get a fresh IP assignment.
    esp_netif_dhcpc_stop(s_sta_netif);  // May fail if already stopped, that's OK
    esp_netif_ip_info_t zero_ip = {0};
    esp_netif_set_ip_info(s_sta_netif, &zero_ip);
    esp_netif_dhcpc_start(s_sta_netif);
    ESP_LOGI(TAG, "Reusing netif - DHCP client restarted for fresh IP");
  }
  
  // Register WiFi event handlers BEFORE starting WiFi
  // (must be done before esp_wifi_start so we catch STA_START event)
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_wifi_handler_instance));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_ip_handler_instance));

  // Configure WiFi BEFORE starting it (correct ESP-IDF order)
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  
  // Start WiFi - this triggers WIFI_EVENT_STA_START which calls esp_wifi_connect()
  esp_err_t err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
    // Unregister handlers on failure
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler_instance);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler_instance);
    s_wifi_handler_instance = NULL;
    s_ip_handler_instance = NULL;
    return err;
  }

  // Wait for connection + IP assignment (event-driven, no polling)
  // Timeout: 30 seconds total for connect + DHCP
  ESP_LOGI(TAG, "Waiting for WiFi connection and IP assignment (timeout: 30s)...");
  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group,
      WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
      pdFALSE,   // Don't clear bits on exit
      pdFALSE,   // Wait for ANY bit (connected OR fail)
      pdMS_TO_TICKS(30000));

  if (bits & WIFI_CONNECTED_BIT) {
    // Disable WiFi power management for reliable bulk transfer.
    // Without this, the STA radio sleeps between TCP segments causing
    // beacon timeouts and stalled uploads (especially for large payloads).
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Successfully connected and got IP
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
      ESP_LOGI(TAG, "Connected to AP: %s (RSSI: %d, Channel: %d)", 
               ap_info.ssid, ap_info.rssi, ap_info.primary);
    }
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGE(TAG, "Failed to connect to AP after %d retries", WIFI_MAX_RETRIES);
    // Unregister handlers
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler_instance);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler_instance);
    s_wifi_handler_instance = NULL;
    s_ip_handler_instance = NULL;
    return ESP_FAIL;
  } else {
    ESP_LOGE(TAG, "WiFi connection timed out (30s)");
    // Unregister handlers
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler_instance);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler_instance);
    s_wifi_handler_instance = NULL;
    s_ip_handler_instance = NULL;
    return ESP_FAIL;
  }
  
  // Verify IP is assigned (should already be set by GOT_IP event)
  esp_netif_ip_info_t ip_info;
  if (esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
    ESP_LOGE(TAG, "Connected but no IP address assigned");
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler_instance);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler_instance);
    s_wifi_handler_instance = NULL;
    s_ip_handler_instance = NULL;
    return ESP_FAIL;
  }
  
  ESP_LOGI(TAG, "IP: " IPSTR ", GW: " IPSTR ", Netmask: " IPSTR,
           IP2STR(&ip_info.ip), IP2STR(&ip_info.gw), IP2STR(&ip_info.netmask));
  
  // Discover gateway IP
  char gateway_ip[16];
  if (discover_gateway_ip(gateway_ip, sizeof(gateway_ip)) != ESP_OK) {
    ESP_LOGE(TAG, "Gateway discovery failed");
    return ESP_FAIL;
  }
  
  // Configure URLs
  snprintf(g_uav_server_url_onboard, sizeof(g_uav_server_url_onboard),
           "http://%s:%d/onboard", gateway_ip, UAV_SERVER_PORT);
  snprintf(g_uav_server_url_data, sizeof(g_uav_server_url_data), 
           "http://%s:%d/data", gateway_ip, UAV_SERVER_PORT);
  snprintf(g_uav_server_url_ack, sizeof(g_uav_server_url_ack),
           "http://%s:%d/ack", gateway_ip, UAV_SERVER_PORT);
  
  ESP_LOGI(TAG, "UAV server URLs configured:");
  ESP_LOGI(TAG, "  Onboard: %s", g_uav_server_url_onboard);
  ESP_LOGI(TAG, "  Data: %s", g_uav_server_url_data);
  ESP_LOGI(TAG, "  Ack: %s", g_uav_server_url_ack);
  
  return ESP_OK;
}

esp_err_t uav_client_run_onboarding(void) {
  s_onboard_session_count++;
  ESP_LOGI(TAG, "Starting UAV Onboarding Sequence (session #%lu)", s_onboard_session_count);

  // 1. Connect to Wi-Fi
  if (wifi_join(UAV_WIFI_SSID, UAV_WIFI_PASS) != ESP_OK) {
    return ESP_FAIL;
  }

  // 2. Prepare Data
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);

  const char *node_id = "wsn_node";              // Dynamic?
  const char *metadata = "lat=7.123;lon=80.456"; // Placeholder

  char token[65];
  generate_token(node_id, metadata, token);

  // 3. POST /onboard
  char resp_buf[MAX_HTTP_RESPONSE_SIZE] = {0};
  http_response_t resp_data = {
    .buffer = resp_buf,
    .len = 0,
    .max_len = sizeof(resp_buf),
  };

  esp_http_client_config_t config = {
      .url = g_uav_server_url_onboard,  // Use dynamic URL
      .method = HTTP_METHOD_POST,
      .timeout_ms = 5000,
      .event_handler = _http_event_handler,
      .user_data = &resp_data,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_http_client_set_header(client, "Content-Type", "application/json");

  char post_data[256];
  snprintf(post_data, sizeof(post_data),
           "{\"node_id\":\"%s\",\"mac\":\"%s\",\"token\":\"%s\",\"metadata\":"
           "\"%s\"}",
           node_id, mac_str, token, metadata);

  esp_http_client_set_post_field(client, post_data, strlen(post_data));

  esp_err_t err = esp_http_client_perform(client);
  char session_id[64] = {0};
  bool success = false;

  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Onboard Status: %d", status_code);
    if (status_code == 200) {
      ESP_LOGI(TAG, "Response (%d bytes): %s", resp_data.len, resp_buf);

      cJSON *json = cJSON_Parse(resp_buf);
      if (json) {
        cJSON *sid = cJSON_GetObjectItem(json, "session_id");
        if (cJSON_IsString(sid) && (sid->valuestring != NULL)) {
          strncpy(session_id, sid->valuestring, sizeof(session_id) - 1);
          ESP_LOGI(TAG, "Session ID: %s", session_id);
          success = true;
        } else {
          ESP_LOGE(TAG, "No session_id in response");
        }
        cJSON_Delete(json);
      } else {
        ESP_LOGE(TAG, "Failed to parse JSON response");
      }
    }
  } else {
    ESP_LOGE(TAG, "Onboard POST failed: %s", esp_err_to_name(err));
  }
  esp_http_client_cleanup(client);

  // 4. Upload Data (if session created)
  if (success) {
    ESP_LOGI(TAG, "Uploading stored data...");

    // Dynamic buffer allocation based on file size
    size_t buf_size = 4096;  // Default size
    struct stat st;
    
    // Check compressed file size first
    if (stat("/spiffs/data.lz", &st) == 0) {
      // Estimate decompressed size (worst case: no compression, add 50% margin)
      buf_size = st.st_size * 2;
      if (buf_size > MAX_BUFFER_SIZE) buf_size = MAX_BUFFER_SIZE;
      if (buf_size < 4096) buf_size = 4096;  // Minimum 4KB
      ESP_LOGI(TAG, "Compressed file size: %zu bytes, allocating %zu bytes buffer", 
               (size_t)st.st_size, buf_size);
    } else if (stat("/spiffs/data.txt", &st) == 0) {
      // Plain-text file
      buf_size = st.st_size + 1024;  // Add margin
      if (buf_size > MAX_BUFFER_SIZE) buf_size = MAX_BUFFER_SIZE;
      ESP_LOGI(TAG, "Plain-text file size: %zu bytes, allocating %zu bytes buffer",
               (size_t)st.st_size, buf_size);
    }

    // Use decompressed read (handles both compressed and plain-text)
    char *log_buffer = NULL;
    size_t bytes_read = 0;
    esp_err_t read_err = storage_manager_read_all_decompressed(&log_buffer, buf_size, &bytes_read);
    
    bool has_data = (read_err == ESP_OK && log_buffer && bytes_read > 0);
    
    // Determine what to send
    const char *upload_data = NULL;
    size_t upload_len = 0;
    const char *content_type = NULL;
    const char *empty_payload = "{\"status\":\"empty\",\"message\":\"no sensor data collected yet\"}";
    
    if (has_data) {
      ESP_LOGI(TAG, "Read %zu bytes of data (decompressed)", bytes_read);
      upload_data = log_buffer;
      upload_len = bytes_read;
      content_type = "text/plain";
    } else {
      // No data available - send explicit empty payload so server knows
      if (read_err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No stored data yet - sending empty payload to server");
      } else {
        ESP_LOGW(TAG, "Failed to read data (%s) - sending empty payload", esp_err_to_name(read_err));
      }
      upload_data = empty_payload;
      upload_len = strlen(empty_payload);
      content_type = "application/json";
    }

    esp_http_client_config_t data_config = {
        .url = g_uav_server_url_data,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
    };
    client = esp_http_client_init(&data_config);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "X-Session-ID", session_id);
    // Tell server whether this is real data or empty
    esp_http_client_set_header(client, "X-Data-Status", has_data ? "data" : "empty");

    // Use open/write API for chunked transfer instead of perform().
    // perform() buffers the entire payload internally and can stall on
    // a constrained TCP window (SND_BUF=5760) with large payloads (36KB+).
    // The open/write loop sends in 4KB chunks with proper TCP backpressure.
    err = esp_http_client_open(client, (int)upload_len);
    if (err == ESP_OK) {
      // Write body in chunks
      const int CHUNK_SIZE = 4096;
      size_t bytes_sent = 0;
      while (bytes_sent < upload_len) {
        int to_send = (int)((upload_len - bytes_sent > CHUNK_SIZE) ? CHUNK_SIZE : (upload_len - bytes_sent));
        int written = esp_http_client_write(client, upload_data + bytes_sent, to_send);
        if (written < 0) {
          ESP_LOGE(TAG, "Data Upload write error at %zu/%zu bytes", bytes_sent, upload_len);
          err = ESP_FAIL;
          break;
        }
        bytes_sent += (size_t)written;
      }

      if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sent %zu/%zu bytes, reading response...", bytes_sent, upload_len);
        int content_length = esp_http_client_fetch_headers(client);
        int code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Data Upload Status: %d (%s), response_len=%d",
                 code, has_data ? "with data" : "empty payload", content_length);
        if (code == 200) {
          if (has_data) {
            // Clear storage only when real data was successfully uploaded
            storage_manager_clear();
          }
        } else {
          ESP_LOGE(TAG, "Server rejected data upload: HTTP %d", code);
          success = false;
        }
      } else {
        ESP_LOGE(TAG, "Data Upload Failed during write: %s", esp_err_to_name(err));
        success = false;
      }
    } else {
      ESP_LOGE(TAG, "Data Upload Failed to open connection: %s", esp_err_to_name(err));
      success = false;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    // Free the heap-allocated buffer (only if we had real data)
    if (log_buffer) {
      free(log_buffer);
    }
  }

  // 5. POST /ack
  if (success) {
    esp_http_client_config_t ack_config = {
        .url = g_uav_server_url_ack,  // Use dynamic URL
        .method = HTTP_METHOD_POST,
    };
    client = esp_http_client_init(&ack_config);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    snprintf(post_data, sizeof(post_data), "{\"session_id\":\"%s\"}",
             session_id);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    err = esp_http_client_perform(client);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "ACK Sent. Status: %d",
               esp_http_client_get_status_code(client));
    }
    esp_http_client_cleanup(client);
  }

  return success ? ESP_OK : ESP_FAIL;
}

// --- Cleanup Function (call before reinitializing ESP-NOW) ---
void uav_client_cleanup(void) {
  // NOTE: We do NOT destroy the netif here because:
  // 1. The WiFi driver has internal event handlers registered to the netif
  // 2. Destroying it causes crash when esp_wifi_start() is called again
  // 3. The netif can be safely reused for future WiFi connections
  //
  // We only disconnect and stop WiFi - reinit will restart it for ESP-NOW
  
  ESP_LOGI(TAG, "Cleaning up UAV client (session #%lu) - disconnecting WiFi", s_onboard_session_count);
  
  // Unregister WiFi event handlers first (prevent stale callbacks during teardown)
  if (s_wifi_handler_instance) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler_instance);
    s_wifi_handler_instance = NULL;
  }
  if (s_ip_handler_instance) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler_instance);
    s_ip_handler_instance = NULL;
  }
  
  // Disconnect from AP
  esp_wifi_disconnect();
  
  // Stop WiFi (will be restarted by esp_now_manager_reinit)
  esp_err_t err = esp_wifi_stop();
  if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
    ESP_LOGW(TAG, "WiFi stop warning: %s", esp_err_to_name(err));
  }
  
  // Clear stale IP info on netif so next session gets fresh DHCP
  if (s_sta_netif) {
    // Stop DHCP client to ensure clean restart next time
    esp_netif_dhcpc_stop(s_sta_netif);
    esp_netif_ip_info_t zero_ip = {0};
    esp_netif_set_ip_info(s_sta_netif, &zero_ip);
  }
  
  // Clear URL globals to prevent stale usage
  g_uav_server_url_onboard[0] = '\0';
  g_uav_server_url_data[0] = '\0';
  g_uav_server_url_ack[0] = '\0';
  
  // Keep s_sta_netif and s_wifi_event_group - they will be reused on next connection
  ESP_LOGI(TAG, "WiFi stopped, netif preserved for reuse (total sessions: %lu)", s_onboard_session_count);
}
