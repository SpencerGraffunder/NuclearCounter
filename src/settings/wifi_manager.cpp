#include "wifi_manager.h"
#include "config/config.h"
#include <esp_wifi.h>

WiFiManager::WiFiManager() {
    // Constructor
}

bool WiFiManager::setupAP() {
    Serial.println("=== Starting WiFi AP Setup ===");

    // WiFi was pre-initialized in main.cpp BEFORE timing task starts.
    // This prevents the high-priority timing task from starving WiFi init.
    // Now we reconfigure it with proper SSID, IP, and settings.

    // CRITICAL: On ESP32-S3, if AP is already running, we must stop it first
    // before restarting with new configuration, otherwise it crashes in ieee80211_hostap_attach
    if (WiFi.getMode() & WIFI_AP) {
        Serial.println("Stopping existing AP before reconfiguration...");
        WiFi.softAPdisconnect(true);  // true = delete the AP interface
        delay(100);  // Give WiFi stack time to clean up
    }

    // Use the Hertz Hunter per-environment SSID/password (WIFI_SSID /
    // WIFI_PASSWORD build flags from platformio.ini) rather than the
    // MAC-suffixed open network from the original StarForge design.
    WiFi.mode(WIFI_AP);
    delay(50);  // Small delay to ensure mode change is processed

    _apSSID = WIFI_SSID;
    Serial.printf("AP SSID: %s\n", _apSSID.c_str());

    // For AP mode we want maximum stability, not power saving
    WiFi.setSleep(false);

    // Configure AP IP settings
    // 192.168.8.x (NOT 192.168.4.x) — 192.168.4.x collides with the home
    // network (DeerFiber 192.168.4.0/22) and breaks the Mac's routing to it.
    WiFi.softAPConfig(
        IPAddress(192, 168, 8, 1),
        IPAddress(192, 168, 8, 1),
        IPAddress(255, 255, 255, 0)
    );

    delay(200); // Delay 200ms to ensure the AP is ready

    // Force MAX TX power (20 dBm). The board's NVS holds no WiFi PHY
    // calibration, so the driver's default TX power is too low and the AP
    // beacon is invisible to other devices. Must be set after WiFi.mode()
    // and before softAP() to take effect.
    esp_wifi_set_max_tx_power(20);

    Serial.printf("Starting AP with SSID: %s\n", _apSSID.c_str());

    // Start AP with configured settings
    bool ap_started = WiFi.softAP(_apSSID.c_str(), WIFI_PASSWORD, 1, 0, 4);

    if (ap_started) {
        Serial.println("=== WiFi AP Started ===");
        Serial.printf("SSID: %s\n", _apSSID.c_str());
        Serial.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());

        // Set WiFi protocol AFTER AP is started
        esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11N);

        return true;
    } else {
        Serial.println("ERROR: WiFi AP failed to start");
        return false;
    }
}
