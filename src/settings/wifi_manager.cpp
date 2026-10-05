#include "wifi_manager.h"
#include "config/config.h"
#include <esp_wifi.h>

WiFiManager::WiFiManager() {
    // Constructor
}

bool WiFiManager::setupAP() {
    VPRINT("=== Starting WiFi AP Setup ===\n");
    TTRACE("setupAP: begin");

    // WiFi was pre-initialized in main.cpp BEFORE timing task starts.
    // This prevents the high-priority timing task from starving WiFi init.
    // Now we reconfigure it with proper SSID, IP, and settings.

    // CRITICAL: On ESP32-S3, if AP is already running, we must stop it first
    // before restarting with new configuration, otherwise it crashes in ieee80211_hostap_attach
    if (WiFi.getMode() & WIFI_AP) {
        VPRINT("Stopping existing AP before reconfiguration...\n");
        WiFi.softAPdisconnect(true);  // true = delete the AP interface
        delay(100);  // Give WiFi stack time to clean up
    }

    // Use the per-environment SSID/password (WIFI_SSID / WIFI_PASSWORD
    // build flags from platformio.ini).
    WiFi.mode(WIFI_AP);
    TTRACE("setupAP: WiFi.mode(WIFI_AP) returned");
    delay(50);  // Small delay to ensure mode change is processed

    _apSSID = WIFI_SSID;
    VPRINT("AP SSID: %s\n", _apSSID.c_str());

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

    // 200 ms here used to be "ensure the AP is ready" — softAPConfig() is
    // synchronous, so the wait bought nothing and cost 200 ms on every timer
    // entry. 50 ms after the mode change is what the driver actually needs.
    delay(50);
    TTRACE("setupAP: after settle");

    // Force MAX TX power. GOTCHA: esp_wifi_set_max_tx_power() takes 0.25 dBm
    // units, range [8, 84] = 2 dBm .. 20 dBm. Passing 20 (as the original SFOS
    // code did) means 5 dBm — i.e. it LOWERED power ~12 dB below the framework
    // default (CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=20 dBm on both S3 and C3),
    // which is the likely reason the AP beacon was hard to see. 84 = 20 dBm.
    // Must be set after WiFi.mode() and before softAP() to take effect.
    // Verified by readback below so a wrong unit shows up in the log.
    esp_err_t txErr = esp_wifi_set_max_tx_power(84);
    if (txErr != ESP_OK) Serial.printf("TX power set failed: %d\n", (int)txErr);

    // Start AP with configured settings
    bool ap_started = WiFi.softAP(_apSSID.c_str(), WIFI_PASSWORD, 1, 0, 4);
    TTRACE("setupAP: softAP returned");

    if (ap_started) {
        VPRINT("=== WiFi AP Started ===\n");
        VPRINT("SSID: %s\n", _apSSID.c_str());
        VPRINT("IP: %s\n", WiFi.softAPIP().toString().c_str());

        // Set WiFi protocol AFTER AP is started
        esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11N);

        // Readback AFTER the radio is started (esp_wifi_get_max_tx_power returns
        // an error before esp_wifi_start). Confirms the 0.25 dBm-unit value took
        // effect — expected 84 = 20.00 dBm.
        int8_t actual = 0;
        if (esp_wifi_get_max_tx_power(&actual) == ESP_OK) {
            VPRINT("TX power in effect: %d (0.25dBm units) = %.2f dBm\n", actual, actual * 0.25);
        }

        return true;
    } else {
        // Worth printing even in production: this is a real failure the user
        // would otherwise see only as "the timer page never comes up".
        Serial.println(F("WiFi AP start failed"));
        return false;
    }
}
