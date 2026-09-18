// Framing confirmed against a REAL captured exchange: a genuine CMD_PROBE ->
// CMD_PROBE_CHALLENGE -> challenge-answer -> CMD_PROBE(ProbeReply) run against
// a real scale, decoded with the same TLV/frame layout as below. The captured
// challenge, that scale's real `kl` and this file's
// SHA1(challenge || mac || kl) formula reproduce the device's own captured
// answer bytes exactly, byte for byte. The same capture is where the deviation
// below comes from: the outbound ProbeChallenge carries 16 ZERO bytes rather
// than random ones, and the scale accepts it.

#include "withings_wpp.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <utility>
#include <sys/stat.h>

#include "esphome/core/log.h"

namespace esphome {
namespace withings_wpp {

static const char *const TAG = "withings_wpp";

// Fixed mount point of the sd_mmc_card component (not configurable there). A
// capture in progress lives under <SD_ROOT>/.partial/withings/<mac>/ until
// CMD_SYNC_OK is acknowledged; see promote_session_file_().
static const char *const SD_ROOT = "/sdcard";

static std::string wpp_partial_dir(const std::string &mac) { return std::string(SD_ROOT) + "/.partial/withings/" + mac; }
static std::string wpp_partial_path(const std::string &mac, const std::string &session_utc) {
  return wpp_partial_dir(mac) + "/" + session_utc + ".wpp";
}
static std::string wpp_completed_dir(const std::string &mac) { return std::string(SD_ROOT) + "/withings/" + mac; }
static std::string wpp_completed_path(const std::string &mac, const std::string &session_utc) {
  return wpp_completed_dir(mac) + "/" + session_utc + ".wpp";
}

// POSIX mkdir() is not recursive and this SD card's FAT driver has no
// mkdir -p equivalent, so each path component is created in turn. EEXIST is
// expected and not an error.
static void mkdir_p(const std::string &path) {
  size_t pos = 1;  // skip the leading '/'
  while (true) {
    pos = path.find('/', pos);
    const std::string component = pos == std::string::npos ? path : path.substr(0, pos);
    if (mkdir(component.c_str(), 0777) != 0 && errno != EEXIST) {
      ESP_LOGW(TAG, "mkdir(%s) failed: errno=%d (%s)", component.c_str(), errno, strerror(errno));
    }
    if (pos == std::string::npos)
      return;
    pos++;
  }
}

// Colon is a reserved character in a FAT long file name (it's the Windows
// drive-letter separator), and address_str() returns e.g. "AA:BB:CC:DD:EE:FF"
// -- so strip it: fopen() on a colon-bearing path fails.
static std::string mac_for_path(const std::string &addr_str) {
  std::string out;
  out.reserve(addr_str.size());
  for (char c : addr_str) {
    if (c != ':')
      out.push_back(c);
  }
  return out;
}

// ---- TLV / frame helpers -------------------------------------------------

std::vector<WppTlv> wpp_parse_objects(const std::vector<uint8_t> &payload) {
  std::vector<WppTlv> out;
  size_t i = 0;
  while (i + 4 <= payload.size()) {
    const uint16_t type = (static_cast<uint16_t>(payload[i]) << 8) | payload[i + 1];
    const uint16_t len = (static_cast<uint16_t>(payload[i + 2]) << 8) | payload[i + 3];
    if (i + 4 + len > payload.size()) {
      ESP_LOGW(TAG, "TLV object type %u claims %u bytes but only %u remain -- stopping parse", type, len,
                (unsigned) (payload.size() - i - 4));
      break;
    }
    out.push_back({type, payload.data() + i + 4, len});
    i += 4 + len;
  }
  return out;
}

namespace {

void append_u16(std::vector<uint8_t> &out, uint16_t v) {
  out.push_back((v >> 8) & 0xFF);
  out.push_back(v & 0xFF);
}

/// Signed fields (gmtOffset, nextGmtOffset) are written with the same
/// big-endian byte pattern as their bit-identical unsigned value -- the cast
/// preserves that pattern, so one helper covers both.
void append_u32(std::vector<uint8_t> &out, uint32_t v) {
  out.push_back((v >> 24) & 0xFF);
  out.push_back((v >> 16) & 0xFF);
  out.push_back((v >> 8) & 0xFF);
  out.push_back(v & 0xFF);
}

std::string hex_string(const uint8_t *data, size_t len) {
  static const char *const kHex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0x0F]);
  }
  return out;
}

/// Friendly name for a StoredMeasureData `type` field, or nullptr for one
/// not in the known list (see the component README's type table). An
/// unrecognised type is still decoded, logged and kept -- callers fall back
/// to logging "type N" rather than dropping it, never to skipping it.
const char *measurement_type_name(uint16_t type) {
  switch (type) {
    case 1:
      return "weight kg";
    case 8:
      return "fat_mass kg";
    case 76:
      return "muscle_mass kg";
    case 77:
      return "hydration kg";
    case 88:
      return "bone_mass kg";
    case 78:
    case 79:
    case 86:
    case 16:
    case 80:
      return "raw impedance";
    case 11:
      return "pulse bpm";
    default:
      return nullptr;
  }
}

void append_object(std::vector<uint8_t> &out, uint16_t type, const std::vector<uint8_t> &data) {
  append_u16(out, type);
  append_u16(out, static_cast<uint16_t>(data.size()));
  out.insert(out.end(), data.begin(), data.end());
}

/// One-byte length prefix, per the framing spec for strings/arrays inside a TLV.
std::vector<uint8_t> length_prefixed(const std::vector<uint8_t> &data) {
  std::vector<uint8_t> out;
  out.push_back(static_cast<uint8_t>(data.size()));
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

std::vector<uint8_t> build_frame(uint16_t cmd, const std::vector<uint8_t> &objects) {
  std::vector<uint8_t> f;
  f.reserve(5 + objects.size());
  f.push_back(0x01);
  append_u16(f, cmd);
  append_u16(f, static_cast<uint16_t>(objects.size()));
  f.insert(f.end(), objects.begin(), objects.end());
  return f;
}

const char *wpp_err_name(int32_t err) {
  // Error constants come from the decompiled app's own object dump (wpp.json
  // in withouthings); the component README has the table.
  switch (err) {
    case -3:
      return "-3 ERR_VAL (bad/missing object in the command)";
    case -5:
      return "-5 NOT_AUTH (no association / probe sequence wrong)";
    case -6:
      return "-6 AUTH_ERR (wrong association key)";
    default:
      return "unrecognized error code";
  }
}

}  // namespace

void WppFrameCodec::feed(const uint8_t *data, size_t len) { this->buffer_.insert(this->buffer_.end(), data, data + len); }

bool WppFrameCodec::has_frame() const {
  if (this->buffer_.size() < 5)
    return false;
  const uint16_t payload_len = (static_cast<uint16_t>(this->buffer_[3]) << 8) | this->buffer_[4];
  return this->buffer_.size() >= 5u + payload_len;
}

WppFrame WppFrameCodec::take_frame() {
  WppFrame frame;
  if (!this->has_frame())
    return frame;
  const uint16_t payload_len = (static_cast<uint16_t>(this->buffer_[3]) << 8) | this->buffer_[4];
  frame.cmd = (static_cast<uint16_t>(this->buffer_[1]) << 8) | this->buffer_[2];
  frame.payload.assign(this->buffer_.begin() + 5, this->buffer_.begin() + 5 + payload_len);
  this->buffer_.erase(this->buffer_.begin(), this->buffer_.begin() + 5 + payload_len);
  return frame;
}

// ---- SHA-1 ----------------------------------------------------------------

namespace {

struct Sha1State {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
};

void sha1_block(Sha1State &st, const uint8_t block[64]) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 80; i++) {
    const uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
    w[i] = (v << 1) | (v >> 31);
  }
  uint32_t a = st.h[0], b = st.h[1], c = st.h[2], d = st.h[3], e = st.h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    const uint32_t temp = (((a << 5) | (a >> 27)) + f + e + k + w[i]);
    e = d;
    d = c;
    c = (b << 30) | (b >> 2);
    b = a;
    a = temp;
  }
  st.h[0] += a;
  st.h[1] += b;
  st.h[2] += c;
  st.h[3] += d;
  st.h[4] += e;
}

}  // namespace

void withings_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
  Sha1State st;
  const size_t full_blocks = len / 64;
  for (size_t i = 0; i < full_blocks; i++)
    sha1_block(st, data + i * 64);

  const size_t rem = len - full_blocks * 64;
  uint8_t buf[128] = {0};
  memcpy(buf, data + full_blocks * 64, rem);
  buf[rem] = 0x80;
  const size_t pad_blocks = (rem < 56) ? 1 : 2;
  const uint64_t bitlen = static_cast<uint64_t>(len) * 8;
  for (int i = 0; i < 8; i++)
    buf[pad_blocks * 64 - 1 - i] = (bitlen >> (8 * i)) & 0xFF;
  for (size_t i = 0; i < pad_blocks; i++)
    sha1_block(st, buf + i * 64);

  for (int i = 0; i < 5; i++) {
    out[i * 4] = (st.h[i] >> 24) & 0xFF;
    out[i * 4 + 1] = (st.h[i] >> 16) & 0xFF;
    out[i * 4 + 2] = (st.h[i] >> 8) & 0xFF;
    out[i * 4 + 3] = st.h[i] & 0xFF;
  }
}

// ---- WithingsWpp ------------------------------------------------------------

void WithingsWpp::dump_config() {
  ESP_LOGCONFIG(TAG, "Withings WPP:");
  ESP_LOGCONFIG(TAG, "  MAC Address: %s", this->parent()->address_str());
}

void WithingsWpp::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                       esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_DISCONNECT_EVT: {
      if (this->state_ != State::IDLE && this->state_ != State::DONE) {
        // finish_() always sets DONE before it disconnects, so landing here
        // still mid-handshake means the link dropped out from under us (peer,
        // or the ESP_GATT_CONN_CONN_CANCEL flakiness the README describes),
        // not that we asked for it.
        ESP_LOGW(TAG, "[%s] Disconnected mid-handshake (state=%d) -- link dropped before finishing",
                 this->parent()->address_str(), (int) this->state_);
      }
      this->state_ = State::IDLE;
      this->char_handle_ = 0;
      this->codec_.reset();
      this->cancel_timeout("write");
      this->cancel_timeout("reply");
      // A link dropped mid-sync is exactly the "short or failed session" case
      // .partial/ exists for: fclose() flushes whatever was captured so far,
      // but promote_session_file_() (the rename into place) only ever runs
      // from handle_sync_ok_ack_(), so an incomplete session is never
      // promoted and never deleted -- see finish_()'s comment.
      if (this->session_file_ != nullptr) {
        fclose(this->session_file_);
        this->session_file_ = nullptr;
      }
      this->pending_measurements_.clear();
      this->current_measurement_index_ = -1;
      break;
    }

    case ESP_GATTC_OPEN_EVT: {
      if (param->open.status != ESP_GATT_OK)
        break;
      // REQUEST ENCRYPTION ON EVERY CONNECT, not just at pairing. A bond is
      // key material, not an encrypted link: reconnecting to an already-bonded
      // scale comes up plaintext unless this is called again, and it then
      // silently re-encrypts from the stored LTK with no new passkey. Skip it
      // and the first subscribe fails with Insufficient Authentication, which
      // looks exactly like an unpaired device and sends you off to re-pair a
      // perfectly good bond.
      //
      // Subscribing does NOT escalate security on its own -- the client
      // connects, holds, and the peer never asks for a passkey.
      //
      // The address is taken from the ble_client rather than hardcoded in the
      // YAML that drives it: the scale's random address changes, and a
      // component depending on config it does not own breaks silently when it
      // does.
      esp_bd_addr_t addr;
      const uint64_t a = this->parent()->get_address();
      for (int i = 0; i < 6; i++)
        addr[i] = static_cast<uint8_t>((a >> (8 * (5 - i))) & 0xFF);
      ESP_LOGD(TAG, "[%s] Requesting an encrypted link", this->parent()->address_str());
      esp_ble_set_encryption(addr, ESP_BLE_SEC_ENCRYPT_MITM);
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto *chr = this->parent()->get_characteristic(SERVICE_UUID, CHAR_UUID);
      if (chr == nullptr) {
        ESP_LOGW(TAG, "[%s] WPP service/characteristic not found -- wrong device or firmware?",
                 this->parent()->address_str());
        this->finish_(false, "WPP characteristic not found");
        break;
      }
      this->char_handle_ = chr->handle;
      this->state_ = State::SUBSCRIBING;

      auto status = this->parent()->register_for_notify(this->char_handle_);
      if (status != ESP_OK) {
        ESP_LOGW(TAG, "[%s] register_for_notify failed, status=%d", this->parent()->address_str(), status);
        this->finish_(false, "register_for_notify failed");
      }
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.handle != this->char_handle_) {
        ESP_LOGD(TAG, "[%s] REG_FOR_NOTIFY for unexpected handle %u (want %u) -- ignored",
                 this->parent()->address_str(), param->reg_for_notify.handle, this->char_handle_);
        break;
      }
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "[%s] Local notify registration failed, status=%d", this->parent()->address_str(),
                 (int) param->reg_for_notify.status);
        this->finish_(false, "register_for_notify status not OK");
      }
      // The actual over-the-air subscribe (the CCCD write) completes later as
      // ESP_GATTC_WRITE_DESCR_EVT below -- that's where an encryption problem
      // would actually show up, not here.
      break;
    }

    case ESP_GATTC_WRITE_DESCR_EVT: {
      if (this->state_ != State::SUBSCRIBING)
        break;  // not our CCCD write (or already handled)
      if (param->write.status != ESP_GATT_OK) {
        // Insufficient Authentication here means the exact opposite of what
        // it looks like: the device IS bonded, but esp_ble_set_encryption
        // (fired from the ble_client's on_connect) hasn't finished escalating
        // the link to encrypted yet when this subscribe was attempted -- see
        // the README's gotchas. Do not mistake this for an unpaired device
        // and go re-pair.
        ESP_LOGW(TAG, "[%s] Notify-enable write failed, status=%d%s", this->parent()->address_str(),
                 (int) param->write.status,
                 param->write.status == ESP_GATT_INSUF_AUTHENTICATION ? " (Insufficient Authentication -- link not "
                                                                         "encrypted yet, not an unpaired device)"
                                                                       : "");
        this->finish_(false, "notify-enable write failed");
        break;
      }
      this->node_state = espbt::ClientState::ESTABLISHED;
      this->state_ = State::AWAITING_CHALLENGE;
      this->send_probe_();
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->char_handle_) {
        ESP_LOGV(TAG, "[%s] Notify on unexpected handle %u (want %u) -- ignored", this->parent()->address_str(),
                 param->notify.handle, this->char_handle_);
        break;
      }
      this->codec_.feed(param->notify.value, param->notify.value_len);
      while (this->codec_.has_frame()) {
        this->cancel_timeout("reply");
        auto frame = this->codec_.take_frame();
        // Raw-first: written verbatim before any decode/dispatch below, so a
        // decoder bug never loses the bytes needed to fix and replay it. A
        // no-op before the session file is open (still authenticating) or
        // after it's been promoted -- see write_session_frame_().
        this->write_session_frame_(frame);
        this->handle_frame_(frame.cmd, frame.payload);
      }
      break;
    }

    default:
      break;
  }
}

void WithingsWpp::send_probe_() {
  ESP_LOGI(TAG, "[%s] Subscribed -- sending CMD_PROBE", this->parent()->address_str());
  std::vector<uint8_t> app_probe = {0x01, 0x01, static_cast<uint8_t>(APP_PROBE_VERSION >> 24),
                                     static_cast<uint8_t>(APP_PROBE_VERSION >> 16),
                                     static_cast<uint8_t>(APP_PROBE_VERSION >> 8), static_cast<uint8_t>(APP_PROBE_VERSION)};
  std::vector<uint8_t> objects;
  append_object(objects, T_APP_PROBE, app_probe);
  append_object(objects, T_APP_PROBE_OS_VERSION,
                {static_cast<uint8_t>(APP_PROBE_OS_VERSION >> 8), static_cast<uint8_t>(APP_PROBE_OS_VERSION)});
  this->send_frame_(CMD_PROBE, objects);
}

void WithingsWpp::send_frame_(uint16_t cmd, const std::vector<uint8_t> &objects) {
  this->write_buffer_ = build_frame(cmd, objects);
  this->write_offset_ = 0;
  this->write_next_fragment_();
  this->set_timeout("reply", REPLY_TIMEOUT_MS, [this, cmd]() {
    ESP_LOGW(TAG, "[%s] Timed out waiting for a reply to cmd %u", this->parent()->address_str(), cmd);
    // A timeout is a terminal path like any other -- see finish_()'s comment.
    this->finish_(false, "reply timeout");
  });
}

void WithingsWpp::write_next_fragment_() {
  const size_t remaining = this->write_buffer_.size() - this->write_offset_;
  const size_t chunk = remaining < WRITE_FRAGMENT_SIZE ? remaining : WRITE_FRAGMENT_SIZE;
  auto status = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                          this->char_handle_, chunk, this->write_buffer_.data() + this->write_offset_,
                                          ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "[%s] esp_ble_gattc_write_char failed, status=%d", this->parent()->address_str(), status);
    return;
  }
  this->write_offset_ += chunk;
  if (this->write_offset_ < this->write_buffer_.size()) {
    this->set_timeout("write", WRITE_FRAGMENT_GAP_MS, [this]() { this->write_next_fragment_(); });
  }
}

void WithingsWpp::handle_frame_(uint16_t cmd, const std::vector<uint8_t> &payload) {
  switch (this->state_) {
    case State::AWAITING_CHALLENGE:
      if (cmd == CMD_ERROR) {
        this->handle_cmd_error_(payload);
      } else if (cmd == CMD_PROBE_CHALLENGE) {
        this->handle_probe_challenge_(payload);
      } else {
        ESP_LOGW(TAG, "[%s] Expected CMD_PROBE_CHALLENGE, got cmd=%u -- unexpected reply shape",
                 this->parent()->address_str(), cmd);
        this->finish_(false, "unexpected reply awaiting challenge");
      }
      break;

    case State::AWAITING_REPLY:
      if (cmd == CMD_ERROR) {
        this->handle_cmd_error_(payload);
      } else if (cmd == CMD_PROBE) {
        // AUTHENTICATED: the device answered our challenge with CMD_PROBE
        // (ProbeReply + its own answer to our challenge), not CMD_ERROR. We
        // don't verify the device's own answer back -- proving WE can
        // authenticate TO the scale is enough to proceed. From here on
        // starts the stored-measurement sync.
        ESP_LOGI(TAG, "[%s] AUTHENTICATED -- CMD_PROBE accepted our challenge answer (%u byte reply)",
                 this->parent()->address_str(), (unsigned) payload.size());
        this->start_stored_measure_sync_();
      } else {
        ESP_LOGW(TAG, "[%s] Expected CMD_PROBE (ProbeReply), got cmd=%u -- unexpected reply shape",
                 this->parent()->address_str(), cmd);
        this->finish_(false, "unexpected reply awaiting probe reply");
      }
      break;

    case State::AWAITING_TIME_REPLY:
      if (cmd == CMD_ERROR) {
        this->handle_cmd_error_(payload);
      } else {
        this->handle_time_set_reply_(payload);
      }
      break;

    case State::AWAITING_CONNECT_REASON:
      if (cmd == CMD_ERROR) {
        this->handle_cmd_error_(payload);
      } else {
        this->handle_connect_reason_(payload);
      }
      break;

    case State::AWAITING_STORED_STATUS:
      if (cmd == CMD_ERROR) {
        this->handle_cmd_error_(payload);
      } else {
        this->handle_stored_status_(payload);
      }
      break;

    case State::AWAITING_STORED_DATA:
      if (cmd == CMD_ERROR) {
        this->handle_cmd_error_(payload);
      } else {
        this->handle_stored_data_(cmd, payload);
      }
      break;

    case State::AWAITING_SYNC_OK:
      if (cmd == CMD_ERROR) {
        this->handle_cmd_error_(payload);
      } else {
        this->handle_sync_ok_ack_(payload);
      }
      break;

    default:
      ESP_LOGD(TAG, "[%s] Frame cmd=%u ignored, not mid-handshake (state=%d)", this->parent()->address_str(), cmd,
                (int) this->state_);
      break;
  }
}

void WithingsWpp::handle_cmd_error_(const std::vector<uint8_t> &payload) {
  auto objects = wpp_parse_objects(payload);
  for (const auto &o : objects) {
    if (o.type == T_CMD_ERROR && o.len >= 6) {
      const uint16_t failed_cmd = (static_cast<uint16_t>(o.data[0]) << 8) | o.data[1];
      const int32_t err = static_cast<int32_t>((static_cast<uint32_t>(o.data[2]) << 24) |
                                                (static_cast<uint32_t>(o.data[3]) << 16) |
                                                (static_cast<uint32_t>(o.data[4]) << 8) | o.data[5]);
      const char *name = wpp_err_name(err);
      ESP_LOGW(TAG, "[%s] CMD_ERROR for cmd %u: %s", this->parent()->address_str(), failed_cmd, name);
      this->finish_(false, name);
      return;
    }
  }
  ESP_LOGW(TAG, "[%s] CMD_ERROR frame had no Cmderror object (%u bytes payload)", this->parent()->address_str(),
            (unsigned) payload.size());
  this->finish_(false, "CMD_ERROR with no Cmderror object");
}

void WithingsWpp::handle_probe_challenge_(const std::vector<uint8_t> &payload) {
  auto objects = wpp_parse_objects(payload);
  const WppTlv *challenge_tlv = nullptr;
  for (const auto &o : objects) {
    if (o.type == T_PROBE_CHALLENGE)
      challenge_tlv = &o;
  }
  if (challenge_tlv == nullptr) {
    ESP_LOGW(TAG, "[%s] CMD_PROBE_CHALLENGE had no ProbeChallenge object (%u bytes payload)",
             this->parent()->address_str(), (unsigned) payload.size());
    this->finish_(false, "no ProbeChallenge object");
    return;
  }

  // ProbeChallenge{mac: 1-byte-length-prefixed str, challenge: 1-byte-length-prefixed bytes}.
  const uint8_t *d = challenge_tlv->data;
  const size_t n = challenge_tlv->len;
  size_t i = 0;
  if (i >= n) {
    this->finish_(false, "ProbeChallenge object empty");
    return;
  }
  const uint8_t mac_len = d[i++];
  if (i + mac_len + 1 > n) {
    ESP_LOGW(TAG, "[%s] ProbeChallenge object truncated (mac)", this->parent()->address_str());
    this->finish_(false, "ProbeChallenge truncated (mac)");
    return;
  }
  std::string mac(reinterpret_cast<const char *>(d + i), mac_len);
  i += mac_len;
  const uint8_t challenge_len = d[i++];
  if (i + challenge_len > n) {
    ESP_LOGW(TAG, "[%s] ProbeChallenge object truncated (challenge)", this->parent()->address_str());
    this->finish_(false, "ProbeChallenge truncated (challenge)");
    return;
  }
  std::vector<uint8_t> challenge(d + i, d + i + challenge_len);

  // The device is observed to send this already lowercase; lowercase it
  // defensively anyway, since the SHA1 input has to match byte-for-byte.
  std::transform(mac.begin(), mac.end(), mac.begin(), [](unsigned char c) { return std::tolower(c); });

  ESP_LOGD(TAG, "[%s] Challenge: mac=%s challenge=%u bytes", this->parent()->address_str(), mac.c_str(),
            (unsigned) challenge.size());

  std::vector<uint8_t> sha_input = challenge;
  sha_input.insert(sha_input.end(), mac.begin(), mac.end());
  sha_input.insert(sha_input.end(), this->association_key_.begin(), this->association_key_.end());
  uint8_t answer[20];
  withings_sha1(sha_input.data(), sha_input.size(), answer);

  std::vector<uint8_t> mac_bytes(mac.begin(), mac.end());
  std::vector<uint8_t> objects_out;
  append_object(objects_out, T_PROBE_CHALLENGE_RESP, length_prefixed(std::vector<uint8_t>(answer, answer + 20)));
  // Our own outbound ProbeChallenge: re-sends the device's own identity MAC
  // (not the ESP32's) with an all-zero 16-byte challenge, matching the real
  // capture referenced at the top of this file byte-for-byte -- see there for
  // why this isn't randomized.
  std::vector<uint8_t> our_challenge_obj = length_prefixed(mac_bytes);
  auto our_challenge_lp = length_prefixed(std::vector<uint8_t>(16, 0));
  our_challenge_obj.insert(our_challenge_obj.end(), our_challenge_lp.begin(), our_challenge_lp.end());
  append_object(objects_out, T_PROBE_CHALLENGE, our_challenge_obj);

  this->state_ = State::AWAITING_REPLY;
  this->send_frame_(CMD_PROBE_CHALLENGE, objects_out);
}


// ---- Stored-measurement sync -----------------------------------------------
//
// Fixed sequence once authenticated, matching the openwithings protocol
// notes:
//   CMD_TIME_SET -> CMD_CONNECT_REASON -> STORED_MEASURE{GETSTATE} ->
//   STORED_MEASURE{GETALL} (streams Meta/MetaExtend/Data* per measurement,
//   terminated by Null) -> CMD_SYNC_OK (mandatory; skip it and the scale
//   ignores the next connection). Never DELALL -- see STORED_ACTION_GETALL's
//   comment in the header.

void WithingsWpp::start_stored_measure_sync_() {
  // Session identity for the raw capture, captured once and reused for both
  // the .partial path and (only after CMD_SYNC_OK is acknowledged) the
  // promoted path.
  this->session_mac_ = mac_for_path(this->parent()->address_str());
  time_t utc = 0;
  if (this->time_ == nullptr) {
    ESP_LOGW(TAG, "[%s] No time_id configured -- TimeSet and the session filename will carry utc=0",
             this->parent()->address_str());
  } else {
    auto now = this->time_->now();
    if (now.is_valid()) {
      utc = now.timestamp;
    } else {
      ESP_LOGW(TAG, "[%s] System time not yet synced -- TimeSet and the session filename will carry utc=0",
               this->parent()->address_str());
    }
  }
  // gmtime_r, not the time component's own (locale-adjusted) broken-down
  // fields: the filename is documented as "session-utc" and utc is already
  // the real UTC epoch, so a plain UTC breakdown is simplest and needs no
  // assumption about how the configured time source applies its timezone.
  struct tm tm_utc {};
  gmtime_r(&utc, &tm_utc);
  char buf[16];
  strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &tm_utc);
  this->session_utc_ = buf;

  mkdir_p(wpp_partial_dir(this->session_mac_));
  const std::string path = wpp_partial_path(this->session_mac_, this->session_utc_);
  this->session_file_ = fopen(path.c_str(), "wb");
  if (this->session_file_ == nullptr) {
    // Not fatal to the sync itself -- HA sensors and logs still update, only
    // the raw capture is lost. Said out loud rather than silently proceeding
    // without a file, since the whole point of raw-first is not discovering
    // this after the weigh-in window has closed.
    ESP_LOGW(TAG, "[%s] fopen(%s) failed: errno=%d (%s) -- continuing without a raw capture",
             this->parent()->address_str(), path.c_str(), errno, strerror(errno));
  } else {
    ESP_LOGI(TAG, "[%s] Session capture -> %s", this->parent()->address_str(), path.c_str());
  }

  std::vector<uint8_t> ts_data;
  append_u32(ts_data, static_cast<uint32_t>(utc));
  append_u32(ts_data, static_cast<uint32_t>(GMT_OFFSET_SECONDS));  // gmtOffset i32
  append_u32(ts_data, 0);                                          // dstChangeTime u32 -- no DST in this zone
  append_u32(ts_data, static_cast<uint32_t>(GMT_OFFSET_SECONDS));  // nextGmtOffset i32 -- no transition pending
  std::vector<uint8_t> objects;
  append_object(objects, T_TIME_SET, ts_data);

  this->state_ = State::AWAITING_TIME_REPLY;
  this->send_frame_(CMD_TIME_SET, objects);
}

void WithingsWpp::handle_time_set_reply_(const std::vector<uint8_t> &payload) {
  auto objects = wpp_parse_objects(payload);
  bool found = false;
  int32_t drift = 0;
  for (const auto &o : objects) {
    if (o.type == T_TIME_SET_REPLY && o.len >= 4) {
      drift = static_cast<int32_t>((static_cast<uint32_t>(o.data[0]) << 24) | (static_cast<uint32_t>(o.data[1]) << 16) |
                                    (static_cast<uint32_t>(o.data[2]) << 8) | o.data[3]);
      found = true;
    }
  }
  if (found) {
    ESP_LOGI(TAG, "[%s] Clock drift vs scale: %d s", this->parent()->address_str(), (int) drift);
    if (this->drift_sensor_ != nullptr)
      this->drift_sensor_->publish_state(drift);
  } else {
    // Diagnostic only -- drift bounds how far a measurement's `time` can be
    // trusted, but its absence isn't a WPP-level error (no Cmderror), so the
    // sync continues rather than aborting over a missing diagnostic field.
    ESP_LOGW(TAG, "[%s] TimeSetReply had no drift object (%u bytes payload) -- continuing",
             this->parent()->address_str(), (unsigned) payload.size());
  }

  this->state_ = State::AWAITING_CONNECT_REASON;
  this->send_frame_(CMD_CONNECT_REASON, {});
}

void WithingsWpp::handle_connect_reason_(const std::vector<uint8_t> &payload) {
  auto objects = wpp_parse_objects(payload);
  bool found = false;
  uint16_t reason = 0;
  for (const auto &o : objects) {
    if (o.type == T_CONNECT_REASON && o.len >= 2) {
      reason = (static_cast<uint16_t>(o.data[0]) << 8) | o.data[1];
      found = true;
    }
  }
  const char *reason_name = reason == 1 ? "USER_REQ (button)" : reason == 2 ? "DEVICE_REQ (weigh-in)" : "unknown";
  if (found) {
    ESP_LOGI(TAG, "[%s] Connect reason: %s (%u)", this->parent()->address_str(), reason_name, reason);
  } else {
    ESP_LOGW(TAG, "[%s] ConnectReason had no reason object (%u bytes payload)", this->parent()->address_str(),
             (unsigned) payload.size());
  }
  if (this->connect_reason_text_sensor_ != nullptr)
    this->connect_reason_text_sensor_->publish_state(found ? reason_name : "unknown");

  std::vector<uint8_t> objects_out;
  append_object(objects_out, T_STORED_ACTION, {STORED_ACTION_GETSTATE, 0});
  this->state_ = State::AWAITING_STORED_STATUS;
  this->send_frame_(CMD_STORED_MEASURE, objects_out);
}

void WithingsWpp::handle_stored_status_(const std::vector<uint8_t> &payload) {
  auto objects = wpp_parse_objects(payload);
  bool found = false;
  int16_t cnt = 0;
  int32_t oldest = 0;
  uint8_t wifi_configured = 0;
  for (const auto &o : objects) {
    if (o.type == T_STORED_STATUS && o.len >= 7) {
      cnt = static_cast<int16_t>((static_cast<uint16_t>(o.data[0]) << 8) | o.data[1]);
      oldest = static_cast<int32_t>((static_cast<uint32_t>(o.data[2]) << 24) | (static_cast<uint32_t>(o.data[3]) << 16) |
                                     (static_cast<uint32_t>(o.data[4]) << 8) | o.data[5]);
      wifi_configured = o.data[6];
      found = true;
    }
  }
  if (found) {
    ESP_LOGI(TAG, "[%s] Scale reports %d stored measurement(s), oldest ts=%d, wifiConfigured=%u",
             this->parent()->address_str(), (int) cnt, (int) oldest, wifi_configured);
  } else {
    ESP_LOGW(TAG, "[%s] StoredMeasureStatus had no status object (%u bytes payload) -- requesting GETALL anyway",
             this->parent()->address_str(), (unsigned) payload.size());
  }

  this->pending_measurements_.clear();
  this->current_measurement_index_ = -1;
  this->getall_raw_dumped_ = false;

  std::vector<uint8_t> objects_out;
  append_object(objects_out, T_STORED_ACTION, {STORED_ACTION_GETALL, 0});
  this->state_ = State::AWAITING_STORED_DATA;
  this->send_frame_(CMD_STORED_MEASURE, objects_out);
}

void WithingsWpp::handle_stored_data_(uint16_t cmd, const std::vector<uint8_t> &payload) {
  // HARD REQUIREMENT: dump the raw reply, once, before trusting anything the
  // parser below extracts from it. Framing assumed rather than checked is the
  // expensive failure mode here -- it presents as a phantom "unexpected reply"
  // that no amount of reading the parser explains. LOGI so it survives at the
  // device's default log level during a live test, not LOGD/LOGV.
  if (!this->getall_raw_dumped_) {
    ESP_LOGI(TAG, "[%s] GETALL raw reply (cmd=%u, %u bytes): %s", this->parent()->address_str(), cmd,
             (unsigned) payload.size(), hex_string(payload.data(), payload.size()).c_str());
    this->getall_raw_dumped_ = true;
  }

  auto objects = wpp_parse_objects(payload);
  for (const auto &o : objects) {
    switch (o.type) {
      case T_STORED_META: {
        // StoredMeasureMeta{uid u32, userIdCnt u8, userId{u8 count, u32[count]},
        // attrib u8, time u32}.
        //
        // NOTE THE TWO COUNT BYTES. The reference writes the userId field as
        // "u8 count + u32[]", meaning the array is ITSELF length-prefixed and
        // sits after userIdCnt rather than being sized by it. Reading the
        // array length from userIdCnt instead collapses the array to empty and
        // slides attrib and time twelve bytes left, which reports
        // "users=0 attrib=3 time=0" for every measurement -- plausible-looking
        // and completely wrong. Verified against a real 23-byte
        // capture: 4 + 1 + 1 + (3 x 4) + 1 + 4 = 23, exact, no remainder.
        //
        // Minimum size (empty array) is 11 bytes.
        if (o.len < 11) {
          ESP_LOGW(TAG, "[%s] StoredMeasureMeta too short (%u bytes) -- skipping this measurement",
                   this->parent()->address_str(), (unsigned) o.len);
          break;
        }
        WppMeasurement m;
        m.uid = (static_cast<uint32_t>(o.data[0]) << 24) | (static_cast<uint32_t>(o.data[1]) << 16) |
                (static_cast<uint32_t>(o.data[2]) << 8) | o.data[3];
        const uint8_t user_id_cnt = o.data[4];
        const uint8_t user_id_len = o.data[5];
        const size_t need = 6u + 4u * user_id_len + 1u + 4u;
        if (o.len < need) {
          ESP_LOGW(TAG,
                   "[%s] StoredMeasureMeta uid=%u claims %u userId(s) but only %u bytes present -- "
                   "attrib/time unavailable",
                   this->parent()->address_str(), (unsigned) m.uid, user_id_len, (unsigned) o.len);
        } else {
          m.user_ids.reserve(user_id_len);
          for (uint8_t i = 0; i < user_id_len; i++) {
            const size_t off = 6 + 4 * i;
            m.user_ids.push_back((static_cast<uint32_t>(o.data[off]) << 24) |
                                  (static_cast<uint32_t>(o.data[off + 1]) << 16) |
                                  (static_cast<uint32_t>(o.data[off + 2]) << 8) | o.data[off + 3]);
          }
          const size_t attrib_off = 6 + 4 * user_id_len;
          m.attrib = o.data[attrib_off];
          const size_t time_off = attrib_off + 1;
          m.time = (static_cast<uint32_t>(o.data[time_off]) << 24) | (static_cast<uint32_t>(o.data[time_off + 1]) << 16) |
                   (static_cast<uint32_t>(o.data[time_off + 2]) << 8) | o.data[time_off + 3];
        }
        ESP_LOGI(TAG, "[%s] Measurement uid=%u userIdCnt=%u users=%u attrib=%u time=%u", this->parent()->address_str(),
                 (unsigned) m.uid, (unsigned) user_id_cnt, (unsigned) m.user_ids.size(), m.attrib,
                 (unsigned) m.time);
        this->pending_measurements_.push_back(std::move(m));
        this->current_measurement_index_ = static_cast<int>(this->pending_measurements_.size()) - 1;
        break;
      }

      case T_STORED_META_EXT: {
        if (o.len < 1) {
          ESP_LOGW(TAG, "[%s] StoredMeasureMetaExtend empty", this->parent()->address_str());
          break;
        }
        if (this->current_measurement_index_ < 0) {
          ESP_LOGW(TAG, "[%s] StoredMeasureMetaExtend with no preceding Meta -- ignored",
                   this->parent()->address_str());
          break;
        }
        auto &m = this->pending_measurements_[this->current_measurement_index_];
        m.has_algo = true;
        m.algo = o.data[0];
        ESP_LOGD(TAG, "[%s] Measurement uid=%u algo=%u (%s)", this->parent()->address_str(), (unsigned) m.uid, m.algo,
                  m.algo == 3 ? "impedance measured" : "weight only");
        break;
      }

      case T_STORED_DATA: {
        // StoredMeasureData{value i32, type u16, exponent i16} = 8 bytes.
        if (o.len < 8) {
          ESP_LOGW(TAG, "[%s] StoredMeasureData too short (%u bytes) -- skipping", this->parent()->address_str(),
                   (unsigned) o.len);
          break;
        }
        WppMeasurement::Value v;
        v.value = static_cast<int32_t>((static_cast<uint32_t>(o.data[0]) << 24) |
                                        (static_cast<uint32_t>(o.data[1]) << 16) |
                                        (static_cast<uint32_t>(o.data[2]) << 8) | o.data[3]);
        v.type = (static_cast<uint16_t>(o.data[4]) << 8) | o.data[5];
        v.exponent = static_cast<int16_t>((static_cast<uint16_t>(o.data[6]) << 8) | o.data[7]);
        const double real_value = v.value * std::pow(10.0, v.exponent);
        const char *name = measurement_type_name(v.type);
        // Every value is logged and kept regardless of whether the type is
        // recognised -- an unrecognised type is data, not noise.
        if (name != nullptr) {
          ESP_LOGI(TAG, "[%s]   %s = %.3f", this->parent()->address_str(), name, real_value);
        } else {
          ESP_LOGI(TAG, "[%s]   type %u = %.3f (unrecognised type)", this->parent()->address_str(), v.type,
                   real_value);
        }
        if (this->current_measurement_index_ < 0) {
          ESP_LOGW(TAG, "[%s] StoredMeasureData with no preceding Meta -- value kept in the raw capture only",
                   this->parent()->address_str());
        } else {
          this->pending_measurements_[this->current_measurement_index_].values.push_back(v);
        }
        break;
      }

      case T_NULL: {
        ESP_LOGI(TAG, "[%s] GETALL measurement list complete (Null) -- %u measurement(s) received",
                 this->parent()->address_str(), (unsigned) this->pending_measurements_.size());
        this->publish_measurements_();
        this->state_ = State::AWAITING_SYNC_OK;
        this->send_frame_(CMD_SYNC_OK, {});
        return;  // Null always ends the list -- nothing after it in this frame is ours to parse.
      }

      default:
        // Genuinely unexpected structural TLV (not just an unrecognised
        // measurement *type*, which T_STORED_DATA above already keeps). The
        // raw frame is already durably on the SD card from write_session_frame_()
        // before this point, so this is a visibility log, not the only copy.
        ESP_LOGW(TAG, "[%s] Unrecognised object in GETALL reply: type=%u len=%u data=%s",
                 this->parent()->address_str(), o.type, (unsigned) o.len, hex_string(o.data, o.len).c_str());
        break;
    }
  }
}

void WithingsWpp::publish_measurements_() {
  if (this->pending_measurements_.empty()) {
    ESP_LOGI(TAG, "[%s] No stored measurements to publish", this->parent()->address_str());
    return;
  }
  // GETALL's ordering isn't documented, so the measurement with the largest
  // `time` is trusted over list position for "which one is the latest
  // weigh-in" -- that's the one HA sensors should reflect.
  const WppMeasurement *latest = &this->pending_measurements_.front();
  for (const auto &m : this->pending_measurements_) {
    if (m.time > latest->time)
      latest = &m;
  }
  for (const auto &v : latest->values) {
    const double real_value = v.value * std::pow(10.0, v.exponent);
    switch (v.type) {
      case 1:
        if (this->weight_sensor_ != nullptr)
          this->weight_sensor_->publish_state(real_value);
        break;
      case 8:
        if (this->fat_mass_sensor_ != nullptr)
          this->fat_mass_sensor_->publish_state(real_value);
        break;
      case 76:
        if (this->muscle_mass_sensor_ != nullptr)
          this->muscle_mass_sensor_->publish_state(real_value);
        break;
      case 77:
        if (this->hydration_sensor_ != nullptr)
          this->hydration_sensor_->publish_state(real_value);
        break;
      case 88:
        if (this->bone_mass_sensor_ != nullptr)
          this->bone_mass_sensor_->publish_state(real_value);
        break;
      default:
        // Raw impedance / pulse / anything else: already logged above and
        // preserved in the raw capture, just not exposed as its own sensor.
        break;
    }
  }
}

void WithingsWpp::handle_sync_ok_ack_(const std::vector<uint8_t> & /*payload*/) {
  // The scale's own acknowledgement to CMD_SYNC_OK is what makes a session
  // complete -- not merely having seen Null on GETALL. Promote the raw
  // capture only now; anything short of this ack leaves it in .partial/
  // forever (see promote_session_file_() and the DISCONNECT_EVT case for the
  // abrupt-drop path).
  ESP_LOGI(TAG, "[%s] Scale acknowledged CMD_SYNC_OK -- session complete", this->parent()->address_str());
  this->promote_session_file_();
  if (this->last_sync_text_sensor_ != nullptr)
    this->last_sync_text_sensor_->publish_state(this->session_utc_);
  this->finish_(true, nullptr);
}

void WithingsWpp::write_session_frame_(const WppFrame &frame) {
  if (this->session_file_ == nullptr)
    return;
  const auto raw = build_frame(frame.cmd, frame.payload);
  const size_t written = fwrite(raw.data(), 1, raw.size(), this->session_file_);
  if (written != raw.size()) {
    ESP_LOGW(TAG, "[%s] Session file short fwrite (%u/%u bytes), errno=%d (%s) -- capture may be incomplete",
             this->parent()->address_str(), (unsigned) written, (unsigned) raw.size(), errno, strerror(errno));
  }
}

void WithingsWpp::promote_session_file_() {
  if (this->session_file_ == nullptr) {
    // fopen() failed back at session start -- already warned about there,
    // nothing to promote.
    return;
  }
  // Flush and close before touching the path: renaming while the FAT driver
  // still holds buffered writes for this handle would race the promotion.
  fclose(this->session_file_);
  this->session_file_ = nullptr;

  mkdir_p(wpp_completed_dir(this->session_mac_));
  const std::string src = wpp_partial_path(this->session_mac_, this->session_utc_);
  const std::string dst = wpp_completed_path(this->session_mac_, this->session_utc_);
  if (rename(src.c_str(), dst.c_str()) == 0) {
    ESP_LOGI(TAG, "[%s] Session capture promoted -> %s", this->parent()->address_str(), dst.c_str());
  } else {
    ESP_LOGW(TAG, "[%s] Session capture rename to %s failed, errno=%d (%s) -- left in %s",
             this->parent()->address_str(), dst.c_str(), errno, strerror(errno), src.c_str());
  }
}

bool WithingsWpp::parse_device(const ble_device_base::ESPBTDevice &device) {
  // Match on the WITH service UUID, not the address. The scale advertises a
  // STATIC RANDOM address that changes whenever it reboots, so a hardcoded MAC
  // silently stops matching and the weigh-in is missed with no error anywhere.
  // Its stable identity MAC is carried in the advertised service UUID's node,
  // which is why keying on the service is both more robust and still specific
  // to this device. ("5749 5448" is ASCII "WITH".)
  bool is_scale = false;
  for (auto &uuid : device.get_service_uuids()) {
    // to_str() writes into a caller-supplied buffer rather than returning a
    // string -- the same buffer-based API change ESPHome applied to
    // get_use_address() and url().
    char buf[ble_device_base::UUID_STR_LEN];
    uuid.as_128bit().to_str(buf);
    if (strncmp(buf, "00000020-5749-5448", 18) == 0) {
      is_scale = true;
      break;
    }
  }
  if (!is_scale && device.address_uint64() != this->parent()->get_address())
    return false;

  if (is_scale && device.address_uint64() != this->parent()->get_address()) {
    // The scale rebooted and took a new random address. Re-point the client at
    // it rather than failing to connect for reasons nothing would explain.
    ESP_LOGW(TAG, "Scale address changed to %s -- re-pointing the client", device.address_str().c_str());
    this->parent()->set_address(device.address_uint64());
  }

  // VERBOSE, not VERY_VERBOSE: the device logger runs at VERBOSE and compiles
  // VV statements out entirely, which looks exactly like a listener that is
  // never called. esp32_ble_tracker logs nothing per-advertisement of its own,
  // so without this there is no way to tell the two apart from outside.
  ESP_LOGV(TAG, "[%s] advertisement seen (present=%d)", this->parent()->address_str(), (int) this->scale_present_);

  const bool was_present = this->scale_present_;
  this->scale_present_ = true;
  // Must exceed the 20s BLE connect timeout: the tracker reports no
  // advertisements while a connect is in flight, so a shorter window expires
  // presence during every failed attempt and fakes a fresh edge afterwards.
  this->cancel_timeout("presence");
  this->set_timeout("presence", 60000, [this]() { this->scale_present_ = false; });

  if (was_present)
    return true;

  const uint32_t now = millis();
  if (this->parent()->state() != espbt::ClientState::IDLE) {
    // Said out loud rather than returned silently: an edge is a one-shot, and
    // a client stuck non-IDLE swallows it invisibly until the scale next goes
    // off the air -- which for a device that only advertises after a weigh-in
    // means losing that measurement entirely.
    ESP_LOGD(TAG, "[%s] Weigh-in seen but client state %d, not IDLE", this->parent()->address_str(),
             (int) this->parent()->state());
    return true;
  }
  // Only guards against re-firing inside one advertising burst. The scale is
  // silent between weigh-ins, so this can never suppress a real one.
  if (this->last_attempt_ms_ != 0 && now - this->last_attempt_ms_ < 20000) {
    ESP_LOGD(TAG, "[%s] Weigh-in seen but last attempt %ums ago", this->parent()->address_str(),
             (unsigned) (now - this->last_attempt_ms_));
    return true;
  }

  ESP_LOGI(TAG, "[%s] Weigh-in detected -- connecting", this->parent()->address_str());
  this->last_attempt_ms_ = now;
  this->parent()->connect();
  return true;
}

void WithingsWpp::finish_(bool success, const char *reason) {
  this->state_ = State::DONE;
  if (success) {
    ESP_LOGI(TAG, "[%s] Auth sequence finished: authenticated", this->parent()->address_str());
  } else {
    ESP_LOGW(TAG, "[%s] Auth sequence finished: failed (%s)", this->parent()->address_str(),
              reason != nullptr ? reason : "unknown reason");
  }
  this->cancel_timeout("write");
  this->cancel_timeout("reply");
  // RELEASE THE LINK, unconditionally. A connected BLE peripheral stops
  // advertising, so holding it makes the scale invisible to the next connect
  // attempt -- and, worse, the scale cannot be weighed on at all while a link
  // is held open. Every terminal path (success, protocol error, reply timeout)
  // routes through this function specifically so none of them can skip this.
  if (this->parent() != nullptr) {
    ESP_LOGD(TAG, "[%s] Releasing the link", this->parent()->address_str());
    this->parent()->disconnect();
  }
}

}  // namespace withings_wpp
}  // namespace esphome

#endif  // USE_ESP32
