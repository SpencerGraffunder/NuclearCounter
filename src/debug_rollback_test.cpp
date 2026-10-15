// ---------------------------------------------------------------------------
// Bench-only OTA rollback trigger (env:RBTC3, -DDEBUG_ROLLBACK_TEST=1).
//
// Proves the C3 bootloader's automatic OTA rollback end-to-end WITHOUT WiFi.
//
// The crash image is pre-placed in ota_1 by esptool. tools/build_rbt_image.py
// produces it: a byte-for-byte copy of the good firmware whose entry-point
// instruction is replaced with an illegal RISC-V encoding (0x00000001) and
// whose footer (rolling checksum byte + SHA-256 validation digest) is
// recomputed. esptool `image-info` reports it fully valid (checksum + hash),
// so the bootloader's header validation accepts it — then it traps on the very
// first instruction after the jump to the entry point.
//
// THIS file's only job: make the RUNNING firmware (booted from ota_0) call
// esp_ota_set_boot_partition(ota_1) and reboot. That is the one otadata writer
// the C3 bootloader actually honors — a raw esptool write of the otadata
// partition is rejected ("ota data partition invalid … will try all
// partitions"), which is exactly why the slot switch must go through the API.
//
// esp_ota_set_boot_partition(ota_1) writes otadata as: preferred = ota_1,
// state = PENDING_VERIFY, backup = ota_0 (the running slot). The bootloader
// then boots ota_1; the crash image crash-loops; after the bootloader's retry
// budget it reverts otadata to backup = ota_0 and boots the good image. That
// revert is the automatic rollback under test.
//
// In the RBTC3 build the USB-CDC stream is owned exclusively by rbtDebugPoll()
// below: NodeMode::handleSerialInput() early-returns (see node_mode.cpp), so
// the text command cannot be swallowed by the binary RotorHazard baseline
// protocol. rbtDebugPoll() runs at the very top of loop(), before
// integrated.process().
// ---------------------------------------------------------------------------
#ifdef DEBUG_ROLLBACK_TEST

#include "Arduino.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

static char  rbtLine[16];
static size_t rbtN = 0;

static const char *stateName(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    default:                         return "UNDEFINED";
  }
}

static void rbtDumpStates(const char *when) {
  const esp_partition_t *o0 =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                               ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  const esp_partition_t *o1 =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                               ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
  esp_ota_img_states_t s0 = ESP_OTA_IMG_UNDEFINED;
  esp_ota_img_states_t s1 = ESP_OTA_IMG_UNDEFINED;
  if (o0) esp_ota_get_state_partition(o0, &s0);
  if (o1) esp_ota_get_state_partition(o1, &s1);
  Serial.printf("[RBT] states(%s): ota_0=%s ota_1=%s\n", when,
                o0 ? stateName(s0) : "?", o1 ? stateName(s1) : "?");
}

static void rbtTrigger() {
  Serial.println();
  Serial.println("[RBT] === OTA rollback test: switching boot slot -> ota_1 (crash image) ===");
  rbtDumpStates("before set_boot_partition");

  const esp_partition_t *ota1 =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                               ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
  if (!ota1) {
    Serial.println("[RBT] ERROR: ota_1 partition not found");
    return;
  }
  Serial.printf("[RBT] ota_1 = %s @ 0x%08x (size 0x%x)\n",
                ota1->label, ota1->address, ota1->size);

  esp_err_t err = esp_ota_set_boot_partition(ota1);
  if (err != ESP_OK) {
    Serial.printf("[RBT] ERROR: esp_ota_set_boot_partition -> %d (0x%x)\n",
                  (int)err, (unsigned)err);
    return;
  }

  const esp_partition_t *boot = esp_ota_get_boot_partition();
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(boot, &st);
  Serial.printf("[RBT] boot partition now = %s (state: %d, 1=pending verify)\n",
                boot ? boot->label : "?", (int)st);
  rbtDumpStates("after set_boot_partition");
  Serial.println("[RBT] rebooting in 300 ms ... expect: boot ota_1 -> crash -> "
                 "bootloader rolls back to ota_0");
  delay(300);
  esp_restart();
}

// Parse line-based commands off the USB-CDC stream. Only "RBT" (optionally
// followed by \r) is recognized; everything else is consumed and ignored.
void rbtDebugPoll() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rbtN > 0) {
        rbtLine[rbtN] = 0;
        if (strcmp(rbtLine, "RBT") == 0) {
          rbtTrigger();  // does not return (esp_restart) unless it errored
        }
        rbtN = 0;
      }
    } else if (rbtN < sizeof(rbtLine) - 1) {
      rbtLine[rbtN++] = c;
    }
  }
}

#endif  // DEBUG_ROLLBACK_TEST
