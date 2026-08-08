#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ai_keyboard {

// Wi-Fi UDP 运行时控制面 wire 协议(与 EIAU 音频包同一 UDP 体系,全部小端):
// - EIHB 心跳: 键盘 -> App(audio_host:audio_port)。App 从来源地址学习键盘控制地址。
// - EICC 控制: App -> 键盘(发往心跳来源地址)。start/stop/keepalive,期望 EICA 确认。
// - EICA 确认: 键盘 -> 控制包来源地址,回显 action/session/sequence。
// token 字段为鉴权预留;当前固件按"来源 IP 必须等于已配置 audio_host 解析地址"校验。

inline constexpr std::size_t kHeartbeatPacketBytes = 20;
inline constexpr std::size_t kExtendedHeartbeatPacketBytes = 80;
inline constexpr std::size_t kControlPacketBytes = 36;
inline constexpr std::size_t kControlAckPacketBytes = 20;
inline constexpr std::size_t kControlTokenBytes = 16;

inline constexpr std::uint8_t kAudioControlWireVersion = 1;

enum class AudioControlAction : std::uint8_t {
  Start = 1,
  Stop = 2,
  Keepalive = 3,
};

enum class AudioControlAckStatus : std::uint8_t {
  Ok = 0,
  Unavailable = 1,
  BadRequest = 2,
  Unauthorized = 3,
};

struct HeartbeatFlags {
  bool streaming = false;
  bool audio_ready = false;
};

struct AudioControlCommand {
  AudioControlAction action = AudioControlAction::Keepalive;
  std::uint64_t session_id = 0;
  std::uint32_t sequence = 0;
  std::array<std::uint8_t, kControlTokenBytes> token{};
};

// buffer 至少 kHeartbeatPacketBytes;返回写入字节数。
std::size_t encode_heartbeat(std::uint8_t* buffer,
                             const HeartbeatFlags& flags,
                             std::uint64_t session_id,
                             std::uint32_t sequence);

// 长度/魔数/版本/action 非法时返回 nullopt;session_id 交由上层校验。
std::optional<AudioControlCommand> parse_audio_control(const std::uint8_t* buffer,
                                                       std::size_t length);

// buffer 至少 kControlAckPacketBytes;返回写入字节数。
std::size_t encode_control_ack(std::uint8_t* buffer,
                               AudioControlAction action,
                               AudioControlAckStatus status,
                               std::uint64_t session_id,
                               std::uint32_t sequence);

}  // namespace ai_keyboard
