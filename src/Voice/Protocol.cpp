#include "Voice/Protocol.hpp"
#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>

namespace {

Voice::PacketType ParseType(std::string_view value) {
    if (value == "HELLO") {
        return Voice::PacketType::Hello;
    }
    if (value == "STATE") {
        return Voice::PacketType::State;
    }
    if (value == "FRAME") {
        return Voice::PacketType::Frame;
    }
    if (value == "BYE") {
        return Voice::PacketType::Bye;
    }
    return Voice::PacketType::Unknown;
}

std::string ToTypeString(Voice::PacketType type) {
    switch (type) {
    case Voice::PacketType::Hello:
        return "HELLO";
    case Voice::PacketType::State:
        return "STATE";
    case Voice::PacketType::Frame:
        return "FRAME";
    case Voice::PacketType::Bye:
        return "BYE";
    default:
        return "UNKNOWN";
    }
}

unsigned char NibbleFromHex(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned char>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned char>(10 + (c - 'a'));
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<unsigned char>(10 + (c - 'A'));
    }
    return 0;
}

std::string BytesToHex(const std::vector<unsigned char>& bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.resize(bytes.size() * 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        out[(i * 2) + 0] = kHex[(bytes[i] >> 4) & 0x0F];
        out[(i * 2) + 1] = kHex[bytes[i] & 0x0F];
    }
    return out;
}

std::vector<unsigned char> HexToBytes(std::string_view hex) {
    if ((hex.size() % 2) != 0) {
        return {};
    }
    std::vector<unsigned char> out;
    out.resize(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const unsigned char hi = NibbleFromHex(hex[(i * 2) + 0]);
        const unsigned char lo = NibbleFromHex(hex[(i * 2) + 1]);
        out[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return out;
}

template <typename NumberType>
bool ParseNumber(std::string_view text, NumberType& out) {
    if constexpr (std::is_floating_point_v<NumberType>) {
        try {
            out = static_cast<NumberType>(std::stod(std::string(text)));
            return true;
        } catch (...) {
            return false;
        }
    }
    auto* begin = text.data();
    auto* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc() && ptr == end;
}

} // namespace

namespace Voice {

bool IsVoiceEnvelope(std::string_view packet) {
    return packet.rfind(kEnvelopePrefix, 0) == 0;
}

bool IsSpatialPacket(std::string_view packet) {
    return packet.rfind(kPosPrefix, 0) == 0;
}

std::optional<VoiceEnvelope> ParseVoiceEnvelope(std::string_view packet) {
    if (!IsVoiceEnvelope(packet)) {
        return std::nullopt;
    }
    std::string_view body = packet.substr(kEnvelopePrefix.size());
    std::vector<std::string_view> fields {};
    size_t start = 0;
    while (start <= body.size()) {
        size_t sep = body.find(':', start);
        if (sep == std::string_view::npos) {
            fields.push_back(body.substr(start));
            break;
        }
        fields.push_back(body.substr(start, sep - start));
        start = sep + 1;
    }
    if (fields.size() < 5) {
        return std::nullopt;
    }

    VoiceEnvelope envelope {};
    envelope.type = ParseType(fields[0]);
    if (!ParseNumber(fields[1], envelope.sender_id)) {
        return std::nullopt;
    }
    if (!ParseNumber(fields[2], envelope.sequence)) {
        return std::nullopt;
    }
    if (!ParseNumber(fields[3], envelope.timestamp_ms)) {
        return std::nullopt;
    }
    int active = 0;
    if (!ParseNumber(fields[4], active)) {
        return std::nullopt;
    }
    envelope.voice_active = active != 0;

    if (fields.size() > 5) {
        std::string_view payload = fields[5];
        envelope.payload = HexToBytes(payload);
    }

    return envelope;
}

std::string SerializeVoiceEnvelope(const VoiceEnvelope& envelope) {
    std::string out(kEnvelopePrefix);
    out += ToTypeString(envelope.type);
    out += ":";
    out += std::to_string(envelope.sender_id);
    out += ":";
    out += std::to_string(envelope.sequence);
    out += ":";
    out += std::to_string(envelope.timestamp_ms);
    out += ":";
    out += envelope.voice_active ? "1" : "0";
    if (!envelope.payload.empty()) {
        out += ":";
        out += BytesToHex(envelope.payload);
    }
    return out;
}

std::optional<SpatialPacket> ParseSpatialPacket(std::string_view packet) {
    if (!IsSpatialPacket(packet)) {
        return std::nullopt;
    }
    std::string_view body = packet.substr(kPosPrefix.size());
    std::vector<std::string_view> fields {};
    size_t start = 0;
    while (start <= body.size()) {
        size_t sep = body.find(':', start);
        if (sep == std::string_view::npos) {
            fields.push_back(body.substr(start));
            break;
        }
        fields.push_back(body.substr(start, sep - start));
        start = sep + 1;
    }
    if (fields.size() < 7) {
        return std::nullopt;
    }
    SpatialPacket parsed {};
    if (!ParseNumber(fields[0], parsed.player_id)
        || !ParseNumber(fields[1], parsed.x)
        || !ParseNumber(fields[2], parsed.y)
        || !ParseNumber(fields[3], parsed.z)
        || !ParseNumber(fields[4], parsed.vx)
        || !ParseNumber(fields[5], parsed.vy)
        || !ParseNumber(fields[6], parsed.vz)) {
        return std::nullopt;
    }
    return parsed;
}

std::string SerializeSpatialPacket(const SpatialPacket& packet) {
    std::ostringstream ss;
    ss << kPosPrefix << packet.player_id << ":" << packet.x << ":" << packet.y << ":" << packet.z << ":" << packet.vx << ":" << packet.vy << ":" << packet.vz;
    return ss.str();
}

} // namespace Voice
