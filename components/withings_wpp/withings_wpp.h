#pragma once

// Withings Body Smart scale, connected as a ble_client peer: association
// handshake, then a read-only stored-measurement sync -- see __init__.py's
// docstring for provenance and scope.

#ifdef USE_ESP32

#include <esp_gattc_api.h>
#include <esp_gap_ble_api.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/ble_device_base/ble_device.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/component.h"

namespace esphome {
namespace withings_wpp {

namespace espbt = esphome::esp32_ble_tracker;

// Body Smart = WPP model id 0x0010. Body+ (the well-documented model) is
// 0x0005 with the same "5749-5448" (ASCII "WITH") base and an otherwise
// identical service/characteristic shape -- see the openwithings protocol
// notes (github.com/totruok/openwithings). One characteristic (write,
// write-without-response and notify all on the same handle) carries the
// whole protocol; there is no separate read-only characteristic.
static const espbt::ESPBTUUID SERVICE_UUID = espbt::ESPBTUUID::from_raw("00000020-5749-5448-0010-000000000000");
static const espbt::ESPBTUUID CHAR_UUID = espbt::ESPBTUUID::from_raw("00000024-5749-5448-0010-000000000000");

// Command ids (top-level frame field) actually exercised by the auth
// handshake. Values confirmed against a real captured exchange -- see
// withings_wpp.cpp's header comment.
static const uint16_t CMD_ERROR = 256;
static const uint16_t CMD_PROBE = 257;
static const uint16_t CMD_PROBE_CHALLENGE = 296;

// Command ids for the stored-measurement sync that runs after authentication
// -- taken from the openwithings protocol notes and the object-type dump in
// withouthings' wpp.json (see the component README), not re-derived.
static const uint16_t CMD_TIME_SET = 1281;
static const uint16_t CMD_CONNECT_REASON = 273;
static const uint16_t CMD_STORED_MEASURE = 271;
static const uint16_t CMD_SYNC_OK = 277;

// TLV object type ids used while authenticating.
static const uint16_t T_APP_PROBE = 298;
static const uint16_t T_APP_PROBE_OS_VERSION = 2344;
static const uint16_t T_PROBE_CHALLENGE = 290;
static const uint16_t T_PROBE_CHALLENGE_RESP = 291;
static const uint16_t T_CMD_ERROR = 272;

// TLV object type ids used by the stored-measurement sync. Replies are
// matched by TLV type within the current state, never by the reply frame's
// own `cmd` field -- WPP is documented as strict request/response with
// nothing unsolicited (see WppFrameCodec's comment), so whatever frame
// arrives next while waiting on one of these IS its reply, and object-type
// matching needs no unverified assumption about what number the scale
// happens to put in that reply frame's cmd field.
static const uint16_t T_TIME_SET = 1281;        // request object; TimeSet{utc,gmtOffset,dstChangeTime,nextGmtOffset}
static const uint16_t T_TIME_SET_REPLY = 1282;  // TimeSetReply{drift i32}
static const uint16_t T_CONNECT_REASON = 280;   // ConnectReason{reason u16}: 1 USER_REQ, 2 DEVICE_REQ
static const uint16_t T_STORED_ACTION = 276;    // request object; StoredMeasureAction{cmd u8, rc i8}
static const uint16_t T_STORED_STATUS = 277;    // StoredMeasureStatus{cnt i16, oldestMeasTime i32, wifiConfigured i8}
static const uint16_t T_STORED_META = 278;      // StoredMeasureMeta{uid u32, userIdCnt u8, userId u32[], attrib u8, time u32}
static const uint16_t T_STORED_META_EXT = 299;  // StoredMeasureMetaExtend{algo u8}: 0 weight only, 3 impedance measured
static const uint16_t T_STORED_DATA = 279;      // StoredMeasureData{value i32, type u16, exponent i16}
static const uint16_t T_NULL = 256;             // list terminator -- numerically same as CMD_ERROR, but a TLV type, not a frame cmd

// StoredMeasureAction's command byte. GETALL only, never DELALL: reading the
// scale's stored measurements must never touch what the cloud path depends
// on. DELALL's value (2) is deliberately not defined here so it can't be
// reached for by mistake.
static const uint8_t STORED_ACTION_GETSTATE = 0;
static const uint8_t STORED_ACTION_GETALL = 1;

// The offset CMD_TIME_SET tells the scale to display its own clock in. A
// compile-time constant, not a config option: it is UTC+10 with no DST here,
// which means there is no transition to compute either. Change it (and the
// dstChangeTime/nextGmtOffset fields alongside it in the .cpp) if your zone
// needs something else -- nothing in the decode path depends on it, since
// every measurement's `time` is a UTC epoch.
static const int32_t GMT_OFFSET_SECONDS = 36000;

// AppProbe's app u8 / os u8 / version u32 fields and AppProbeOsVersion's u16 --
// the device's challenge doesn't depend on these being genuine Health Mate
// values, so they're matched byte-for-byte to a real captured exchange rather
// than re-derived.
static const uint32_t APP_PROBE_VERSION = 8070101;
static const uint16_t APP_PROBE_OS_VERSION = 35;

// Fragment size and inter-fragment gap sized for the board's default
// (unnegotiated) ATT MTU of 23 bytes. The largest frame this component ever
// sends (the challenge response) is 69 bytes, so it's several fragments either
// way.
static const size_t WRITE_FRAGMENT_SIZE = 20;
static const uint32_t WRITE_FRAGMENT_GAP_MS = 20;
static const uint32_t REPLY_TIMEOUT_MS = 10000;

/// One TLV object as parsed from a frame's payload -- borrows into the
/// payload it was parsed from, so it may not outlive it.
struct WppTlv {
  uint16_t type{0};
  const uint8_t *data{nullptr};
  size_t len{0};
};

/// One reassembled WPP frame: `01 | cmd(u16 BE) | payload_len(u16 BE) | TLV*`.
struct WppFrame {
  uint16_t cmd{0};
  std::vector<uint8_t> payload;
};

/// One decoded stored measurement: a StoredMeasureMeta plus whatever
/// StoredMeasureMetaExtend/StoredMeasureData objects followed it before the
/// next Meta (or the terminating Null). Parsed in full, not just `uid` --
/// `userId[]` is what makes person attribution possible at all, and `time`
/// is the real measurement time rather than receipt time.
struct WppMeasurement {
  uint32_t uid{0};
  std::vector<uint32_t> user_ids;
  uint8_t attrib{0};
  uint32_t time{0};
  bool has_algo{false};
  uint8_t algo{0};  // 0 = weight only, 3 = impedance measured

  /// One StoredMeasureData: real value = value * 10^exponent.
  struct Value {
    int32_t value{0};
    uint16_t type{0};
    int16_t exponent{0};
  };
  std::vector<Value> values;
};

/// Reassembles notification fragments into complete WPP frames. There is no
/// marker byte to resync on and no CRC -- WPP is a plain request/response
/// protocol over one characteristic with nothing unsolicited, so accumulating
/// until the declared length is satisfied is sufficient.
class WppFrameCodec {
 public:
  /// Feed one notification fragment.
  void feed(const uint8_t *data, size_t len);
  /// True once at least one complete frame is buffered. Loop this + take_frame()
  /// to drain more than one frame that arrived in the same notification.
  bool has_frame() const;
  WppFrame take_frame();
  void reset() { this->buffer_.clear(); }

 protected:
  std::vector<uint8_t> buffer_;
};

/// Parses the TLV objects packed into a frame's payload. Stops (and lets the
/// caller log it) rather than reading out of bounds if a length field would
/// overrun the payload.
std::vector<WppTlv> wpp_parse_objects(const std::vector<uint8_t> &payload);

/// Self-contained SHA-1 (RFC 3174) -- ESP-IDF's mbedtls SHA1 surface differs
/// between the legacy API and the PSA-only API newer IDF majors moved to
/// (see esphome's own sha256 component for exactly this split), so pinning
/// to one now would risk breaking silently on the next framework bump. SHA-1
/// is small enough that a portable implementation costs nothing extra --
/// verified against known test vectors (empty string, "abc", and the 55/56/64
/// byte padding-boundary cases) and against a real captured WPP challenge
/// answer computed with the real `kl`.
void withings_sha1(const uint8_t *data, size_t len, uint8_t out[20]);

/// Connection sequence, advanced purely by gattc_event_handler() and
/// set_timeout() callbacks -- nothing here may block. Begins at
/// ESP_GATTC_SEARCH_CMPL_EVT (the ble_client parent owns connecting and
/// encryption escalation) and ends at DONE, always via finish_(), which
/// always disconnects -- see its comment for why that's non-negotiable.
enum class State : uint8_t {
  IDLE,
  SUBSCRIBING,         // registered for notify, waiting for the CCCD write + REG_FOR_NOTIFY_EVT
  AWAITING_CHALLENGE,  // CMD_PROBE sent, waiting for CMD_PROBE_CHALLENGE (or CMD_ERROR)
  AWAITING_REPLY,      // challenge answer sent, waiting for CMD_PROBE/ProbeReply (or CMD_ERROR)
  // Stored-measurement sync, run in this fixed order once authenticated.
  AWAITING_TIME_REPLY,      // CMD_TIME_SET sent, waiting for TimeSetReply
  AWAITING_CONNECT_REASON,  // CMD_CONNECT_REASON sent, waiting for ConnectReason
  AWAITING_STORED_STATUS,   // GETSTATE sent, waiting for StoredMeasureStatus
  AWAITING_STORED_DATA,     // GETALL sent, accumulating Meta/MetaExtend/Data until Null
  AWAITING_SYNC_OK,         // CMD_SYNC_OK sent, waiting for the scale's ack -- promotion gate
  DONE,                     // finished (synced or failed); link is being/has been released
};

class WithingsWpp : public Component,
                    public ble_client::BLEClientNode,
                    public ble_device_base::ESPBTDeviceListener {
 public:
  // Purely event/timeout-driven; nothing needs a per-tick poll.
  void loop() override { this->disable_loop(); }
  void dump_config() override;

  /// Advertisement listener. The scale is SILENT when idle -- it advertises
  /// only for roughly 55 seconds after someone stands on it, and cannot be
  /// woken remotely. So an advertisement IS the weigh-in event, and the only
  /// workable trigger is to be permanently armed and connect on sight.
  bool parse_device(const ble_device_base::ESPBTDevice &device) override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                            esp_ble_gattc_cb_param_t *param) override;

  void set_association_key(const std::string &kl) { this->association_key_ = kl; }
  void set_time_id(time::RealTimeClock *t) { this->time_ = t; }
  void set_weight_sensor(sensor::Sensor *s) { this->weight_sensor_ = s; }
  void set_fat_mass_sensor(sensor::Sensor *s) { this->fat_mass_sensor_ = s; }
  void set_muscle_mass_sensor(sensor::Sensor *s) { this->muscle_mass_sensor_ = s; }
  void set_hydration_sensor(sensor::Sensor *s) { this->hydration_sensor_ = s; }
  void set_bone_mass_sensor(sensor::Sensor *s) { this->bone_mass_sensor_ = s; }
  void set_drift_sensor(sensor::Sensor *s) { this->drift_sensor_ = s; }
  void set_connect_reason_text_sensor(text_sensor::TextSensor *s) { this->connect_reason_text_sensor_ = s; }
  void set_last_sync_text_sensor(text_sensor::TextSensor *s) { this->last_sync_text_sensor_ = s; }

 protected:
  void send_probe_();
  void send_frame_(uint16_t cmd, const std::vector<uint8_t> &objects);
  void write_next_fragment_();
  void handle_frame_(uint16_t cmd, const std::vector<uint8_t> &payload);
  void handle_probe_challenge_(const std::vector<uint8_t> &payload);
  void handle_cmd_error_(const std::vector<uint8_t> &payload);

  // Stored-measurement sync -- see withings_wpp.cpp for the sequencing
  // comment above start_stored_measure_sync_().
  void start_stored_measure_sync_();
  void handle_time_set_reply_(const std::vector<uint8_t> &payload);
  void handle_connect_reason_(const std::vector<uint8_t> &payload);
  void handle_stored_status_(const std::vector<uint8_t> &payload);
  void handle_stored_data_(uint16_t cmd, const std::vector<uint8_t> &payload);
  void handle_sync_ok_ack_(const std::vector<uint8_t> &payload);
  /// Publishes HA sensor state from whichever measurement in
  /// pending_measurements_ has the latest `time` -- see the .cpp for why
  /// list order isn't trusted instead.
  void publish_measurements_();
  /// Appends one WPP frame's exact wire bytes to session_file_ (a no-op
  /// before the session file is open or after it's been promoted/closed).
  void write_session_frame_(const WppFrame &frame);
  /// Closes session_file_ and renames it from .partial/ into place. Only
  /// called once the scale has acknowledged CMD_SYNC_OK -- see
  /// handle_sync_ok_ack_()'s comment for why that ack, not the Null
  /// terminator, is what defines "complete".
  void promote_session_file_();

  /// Enter DONE and release the link -- every terminal path (success,
  /// protocol error, reply timeout) routes through this. Never just set the
  /// state. `reason` is only used (and may be nullptr) when !success.
  bool scale_present_{false};
  uint32_t last_attempt_ms_{0};
  void finish_(bool success, const char *reason);

  WppFrameCodec codec_;
  State state_{State::IDLE};
  uint16_t char_handle_{0};
  std::string association_key_;
  time::RealTimeClock *time_{nullptr};

  // In-flight request, written in fragments with a gap between each -- see
  // write_next_fragment_() in the .cpp for why.
  std::vector<uint8_t> write_buffer_;
  size_t write_offset_{0};

  // Raw session capture -- one FILE* held open for the whole sync, appended
  // to verbatim before any decode. session_mac_/session_utc_ are captured
  // once at session start and reused to build both the .partial and (once
  // promoted) the final path -- see withings_wpp.cpp's path helpers.
  FILE *session_file_{nullptr};
  std::string session_mac_;
  std::string session_utc_;

  // Measurements accumulated during GETALL, oldest object first as they
  // arrive. current_measurement_index_ is an INDEX, not a pointer/reference,
  // because push_back() may reallocate the vector and invalidate either.
  std::vector<WppMeasurement> pending_measurements_;
  int current_measurement_index_{-1};
  // Guards the one-time raw hex dump of the first GETALL reply -- see
  // handle_stored_data_()'s comment (hard requirement: dump before trusting
  // the decoder).
  bool getall_raw_dumped_{false};

  sensor::Sensor *weight_sensor_{nullptr};
  sensor::Sensor *fat_mass_sensor_{nullptr};
  sensor::Sensor *muscle_mass_sensor_{nullptr};
  sensor::Sensor *hydration_sensor_{nullptr};
  sensor::Sensor *bone_mass_sensor_{nullptr};
  sensor::Sensor *drift_sensor_{nullptr};
  text_sensor::TextSensor *connect_reason_text_sensor_{nullptr};
  text_sensor::TextSensor *last_sync_text_sensor_{nullptr};
};

}  // namespace withings_wpp
}  // namespace esphome

#endif  // USE_ESP32
