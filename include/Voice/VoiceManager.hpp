#pragma once

#include "Voice/Protocol.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Voice {

struct VoiceSettings {
    bool enabled { true };
    bool push_to_talk { true };
    bool follow_system_defaults { true };
    std::string input_device_id { "default" };
    std::string output_device_id { "default" };
    float mic_gain { 1.0f };
    float output_gain { 1.0f };
    float attenuation_near_meters { 10.0f };
    float attenuation_far_meters { 120.0f };
    float attenuation_smoothing { 0.2f };
};

struct SpatialState {
    float x { 0.0f };
    float y { 0.0f };
    float z { 0.0f };
    float vx { 0.0f };
    float vy { 0.0f };
    float vz { 0.0f };
    std::chrono::steady_clock::time_point updated_at {};
};

class VoiceManager {
public:
    static VoiceManager& Get();

    void Start();
    void Stop();
    bool Running() const;

    void SetSettings(const VoiceSettings& settings);
    VoiceSettings GetSettings() const;
    std::string GetSettingsJson() const;
    bool ApplySettingsJson(const std::string& json_text);

    // Applies OS default device tracking unless explicit devices were selected.
    void RefreshDeviceSelection();

    void SetLocalPlayerId(int player_id);
    void SetTalkState(bool active);
    bool TalkState() const;

    void HandleIncoming(const VoiceEnvelope& envelope);
    std::string MakeStatePacket(bool active) const;
    std::string MakeHelloPacket() const;
    std::string MakeByePacket() const;

    void UpdateSpatialState(const SpatialPacket& packet);
    void UpdateSpatialState(int player_id, const SpatialState& state);
    float ComputeAttenuation(int remote_player_id);

private:
    VoiceManager() = default;
    ~VoiceManager() = default;
    VoiceManager(const VoiceManager&) = delete;
    VoiceManager& operator=(const VoiceManager&) = delete;

    void CaptureLoop();
    void PlaybackLoop();
    bool BackendAvailable() const;

    mutable std::mutex m_mutex {};
    bool m_running { false };
    bool m_talking { false };
    int m_local_player_id { -1 };
    uint32_t m_local_sequence { 0 };
    VoiceSettings m_settings {};

    std::unordered_map<int, SpatialState> m_spatial {};
    std::unordered_map<int, float> m_smoothed_gain {};

    std::atomic<bool> m_backend_running { false };
    std::thread m_capture_thread {};
    std::thread m_playback_thread {};
    std::atomic<uint32_t> m_frame_sequence { 0 };

    std::mutex m_audio_mutex {};
    std::condition_variable m_audio_cv {};
    std::unordered_map<int, std::deque<std::vector<float>>> m_jitter_pcm {};
};

} // namespace Voice
