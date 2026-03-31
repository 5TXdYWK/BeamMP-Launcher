#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Voice {

inline constexpr std::string_view kEnvelopePrefix = "VOIP:";
inline constexpr std::string_view kPosPrefix = "VOIP_POS:";

enum class PacketType {
    Hello,
    State,
    Frame,
    Bye,
    Unknown
};

struct VoiceEnvelope {
    PacketType type { PacketType::Unknown };
    int sender_id { -1 };
    uint32_t sequence { 0 };
    uint32_t timestamp_ms { 0 };
    bool voice_active { false };
    std::vector<unsigned char> payload {};
};

struct SpatialPacket {
    int player_id { -1 };
    float x { 0.0f };
    float y { 0.0f };
    float z { 0.0f };
    float vx { 0.0f };
    float vy { 0.0f };
    float vz { 0.0f };
};

bool IsVoiceEnvelope(std::string_view packet);
bool IsSpatialPacket(std::string_view packet);
std::optional<VoiceEnvelope> ParseVoiceEnvelope(std::string_view packet);
std::string SerializeVoiceEnvelope(const VoiceEnvelope& envelope);
std::optional<SpatialPacket> ParseSpatialPacket(std::string_view packet);
std::string SerializeSpatialPacket(const SpatialPacket& packet);

} // namespace Voice
