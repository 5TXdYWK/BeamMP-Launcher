#include "Voice/VoiceTests.hpp"
#include "Logger.h"
#include "Voice/Protocol.hpp"
#include "Voice/VoiceManager.hpp"

namespace Voice {

bool RunSelfTests() {
    VoiceEnvelope env {};
    env.type = PacketType::Frame;
    env.sender_id = 7;
    env.sequence = 42;
    env.timestamp_ms = 1000;
    env.voice_active = true;
    env.payload = { 'a', 'b', 'c' };

    const auto encoded = SerializeVoiceEnvelope(env);
    auto decoded = ParseVoiceEnvelope(encoded);
    if (!decoded.has_value() || decoded->sender_id != 7 || decoded->payload.size() != 3) {
        error("VOIP self-test failed: envelope parse mismatch");
        return false;
    }

    SpatialPacket pos {};
    pos.player_id = 1;
    pos.x = 0.0f;
    pos.y = 0.0f;
    pos.z = 0.0f;
    const auto posEncoded = SerializeSpatialPacket(pos);
    auto posDecoded = ParseSpatialPacket(posEncoded);
    if (!posDecoded.has_value() || posDecoded->player_id != 1) {
        error("VOIP self-test failed: spatial parse mismatch");
        return false;
    }

    auto& manager = VoiceManager::Get();
    manager.SetLocalPlayerId(1);
    manager.Start();

    SpatialState local {};
    local.updated_at = std::chrono::steady_clock::now();
    manager.UpdateSpatialState(1, local);

    SpatialState remote {};
    remote.x = 200.0f;
    remote.updated_at = std::chrono::steady_clock::now();
    manager.UpdateSpatialState(2, remote);

    const float gain = manager.ComputeAttenuation(2);
    manager.Stop();
    if (gain > 0.2f) {
        error("VOIP self-test failed: attenuation too high for far player");
        return false;
    }
    info("VOIP self-tests passed");
    return true;
}

} // namespace Voice
