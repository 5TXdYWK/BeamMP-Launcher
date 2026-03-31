#include "Voice/VoiceManager.hpp"
#include "Logger.h"
#include "Network/network.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <nlohmann/json.hpp>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(BEAMMP_VOIP_HAS_OPUS)
#include <opus/opus.h>
#endif

#if defined(BEAMMP_VOIP_HAS_PORTAUDIO)
#include <portaudio.h>
#endif

namespace {

float Clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

float Distance(const Voice::SpatialState& a, const Voice::SpatialState& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

} // namespace

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 1;
constexpr int kFrameSamples = 960; // 20ms @ 48kHz
constexpr int kOpusMaxBytes = 4000;

#if defined(BEAMMP_VOIP_HAS_OPUS) && defined(BEAMMP_VOIP_HAS_PORTAUDIO)
struct RuntimeAudioState {
    OpusEncoder* encoder { nullptr };
    std::unordered_map<int, OpusDecoder*> decoders {};
    PaStream* input_stream { nullptr };
    PaStream* output_stream { nullptr };
};

RuntimeAudioState& Runtime() {
    static RuntimeAudioState state {};
    return state;
}
#endif

} // namespace

namespace Voice {

VoiceManager& VoiceManager::Get() {
    static VoiceManager manager {};
    return manager;
}

void VoiceManager::Start() {
    {
        std::scoped_lock lock(m_mutex);
        if (m_running) {
            return;
        }
        m_running = true;
    }
    RefreshDeviceSelection();
#if defined(BEAMMP_VOIP_HAS_OPUS) && defined(BEAMMP_VOIP_HAS_PORTAUDIO)
    RuntimeAudioState& rt = Runtime();
    if (Pa_Initialize() != paNoError) {
        warn("VOIP PortAudio init failed, voice runtime disabled");
    } else {
        int opus_err = OPUS_OK;
        rt.encoder = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &opus_err);
        if (!rt.encoder || opus_err != OPUS_OK) {
            warn("VOIP Opus encoder init failed, voice runtime disabled");
        } else {
            opus_encoder_ctl(rt.encoder, OPUS_SET_BITRATE(24000));
            opus_encoder_ctl(rt.encoder, OPUS_SET_COMPLEXITY(5));

            PaStreamParameters in_params {};
            in_params.channelCount = kChannels;
            in_params.sampleFormat = paInt16;
            in_params.hostApiSpecificStreamInfo = nullptr;
            in_params.device = Pa_GetDefaultInputDevice();
            in_params.suggestedLatency = (in_params.device == paNoDevice) ? 0.0 : Pa_GetDeviceInfo(in_params.device)->defaultLowInputLatency;

            PaStreamParameters out_params {};
            out_params.channelCount = kChannels;
            out_params.sampleFormat = paFloat32;
            out_params.hostApiSpecificStreamInfo = nullptr;
            out_params.device = Pa_GetDefaultOutputDevice();
            out_params.suggestedLatency = (out_params.device == paNoDevice) ? 0.0 : Pa_GetDeviceInfo(out_params.device)->defaultLowOutputLatency;

            const bool can_open_input = in_params.device != paNoDevice;
            const bool can_open_output = out_params.device != paNoDevice;
            if (can_open_input && can_open_output
                && Pa_OpenStream(&rt.input_stream, &in_params, nullptr, kSampleRate, kFrameSamples, paClipOff, nullptr, nullptr) == paNoError
                && Pa_OpenStream(&rt.output_stream, nullptr, &out_params, kSampleRate, kFrameSamples, paClipOff, nullptr, nullptr) == paNoError
                && Pa_StartStream(rt.input_stream) == paNoError
                && Pa_StartStream(rt.output_stream) == paNoError) {
                m_backend_running = true;
                m_capture_thread = std::thread(&VoiceManager::CaptureLoop, this);
                m_playback_thread = std::thread(&VoiceManager::PlaybackLoop, this);
                info("VOIP audio runtime started (PortAudio + Opus)");
            } else {
                warn("VOIP audio device open/start failed, runtime disabled");
            }
        }
    }
#endif
    info("VOIP subsystem started");
}

void VoiceManager::Stop() {
    {
        std::scoped_lock lock(m_mutex);
        if (!m_running) {
            return;
        }
        m_running = false;
        m_talking = false;
    }
#if defined(BEAMMP_VOIP_HAS_OPUS) && defined(BEAMMP_VOIP_HAS_PORTAUDIO)
    m_backend_running = false;
    m_audio_cv.notify_all();
    if (m_capture_thread.joinable()) {
        m_capture_thread.join();
    }
    if (m_playback_thread.joinable()) {
        m_playback_thread.join();
    }
    RuntimeAudioState& rt = Runtime();
    if (rt.input_stream) {
        Pa_StopStream(rt.input_stream);
        Pa_CloseStream(rt.input_stream);
        rt.input_stream = nullptr;
    }
    if (rt.output_stream) {
        Pa_StopStream(rt.output_stream);
        Pa_CloseStream(rt.output_stream);
        rt.output_stream = nullptr;
    }
    for (auto& [_, decoder] : rt.decoders) {
        opus_decoder_destroy(decoder);
    }
    rt.decoders.clear();
    if (rt.encoder) {
        opus_encoder_destroy(rt.encoder);
        rt.encoder = nullptr;
    }
    Pa_Terminate();
#endif
    info("VOIP subsystem stopped");
}

bool VoiceManager::Running() const {
    std::scoped_lock lock(m_mutex);
    return m_running;
}

void VoiceManager::SetSettings(const VoiceSettings& settings) {
    std::scoped_lock lock(m_mutex);
    m_settings = settings;
    if (m_settings.follow_system_defaults) {
        m_settings.input_device_id = "default";
        m_settings.output_device_id = "default";
    } else {
        if (m_settings.input_device_id.empty()) {
            m_settings.input_device_id = "default";
            warn("Pinned VOIP input device disappeared, falling back to system default");
        }
        if (m_settings.output_device_id.empty()) {
            m_settings.output_device_id = "default";
            warn("Pinned VOIP output device disappeared, falling back to system default");
        }
    }
}

VoiceSettings VoiceManager::GetSettings() const {
    std::scoped_lock lock(m_mutex);
    return m_settings;
}

std::string VoiceManager::GetSettingsJson() const {
    auto settings = GetSettings();
    nlohmann::json json = {
        { "enabled", settings.enabled },
        { "pushToTalk", settings.push_to_talk },
        { "followSystemDefaults", settings.follow_system_defaults },
        { "inputDeviceId", settings.input_device_id },
        { "outputDeviceId", settings.output_device_id },
        { "micGain", settings.mic_gain },
        { "outputGain", settings.output_gain },
        { "attenuationNearMeters", settings.attenuation_near_meters },
        { "attenuationFarMeters", settings.attenuation_far_meters },
        { "attenuationSmoothing", settings.attenuation_smoothing }
    };
    return json.dump();
}

bool VoiceManager::ApplySettingsJson(const std::string& json_text) {
    auto json = nlohmann::json::parse(json_text, nullptr, false);
    if (json.is_discarded()) {
        return false;
    }
    VoiceSettings next = GetSettings();
    if (json.contains("enabled") && json["enabled"].is_boolean()) {
        next.enabled = json["enabled"].get<bool>();
    }
    if (json.contains("pushToTalk") && json["pushToTalk"].is_boolean()) {
        next.push_to_talk = json["pushToTalk"].get<bool>();
    }
    if (json.contains("followSystemDefaults") && json["followSystemDefaults"].is_boolean()) {
        next.follow_system_defaults = json["followSystemDefaults"].get<bool>();
    }
    if (json.contains("inputDeviceId") && json["inputDeviceId"].is_string()) {
        next.input_device_id = json["inputDeviceId"].get<std::string>();
    }
    if (json.contains("outputDeviceId") && json["outputDeviceId"].is_string()) {
        next.output_device_id = json["outputDeviceId"].get<std::string>();
    }
    if (json.contains("micGain") && json["micGain"].is_number()) {
        next.mic_gain = json["micGain"].get<float>();
    }
    if (json.contains("outputGain") && json["outputGain"].is_number()) {
        next.output_gain = json["outputGain"].get<float>();
    }
    if (json.contains("attenuationNearMeters") && json["attenuationNearMeters"].is_number()) {
        next.attenuation_near_meters = std::max(0.0f, json["attenuationNearMeters"].get<float>());
    }
    if (json.contains("attenuationFarMeters") && json["attenuationFarMeters"].is_number()) {
        next.attenuation_far_meters = std::max(next.attenuation_near_meters + 1.0f, json["attenuationFarMeters"].get<float>());
    }
    if (json.contains("attenuationSmoothing") && json["attenuationSmoothing"].is_number()) {
        next.attenuation_smoothing = Clamp01(json["attenuationSmoothing"].get<float>());
    }
    SetSettings(next);
    return true;
}

void VoiceManager::RefreshDeviceSelection() {
    std::scoped_lock lock(m_mutex);
    // Placeholder for platform audio backend selection.
    // We keep "default" as canonical IDs so launcher's VOIP follows OS defaults unless pinned.
    if (m_settings.follow_system_defaults) {
        m_settings.input_device_id = "default";
        m_settings.output_device_id = "default";
    } else {
        if (m_settings.input_device_id.empty()) {
            m_settings.input_device_id = "default";
            warn("Pinned VOIP input device disappeared, falling back to system default");
        }
        if (m_settings.output_device_id.empty()) {
            m_settings.output_device_id = "default";
            warn("Pinned VOIP output device disappeared, falling back to system default");
        }
    }
}

void VoiceManager::SetLocalPlayerId(int player_id) {
    std::scoped_lock lock(m_mutex);
    m_local_player_id = player_id;
}

void VoiceManager::SetTalkState(bool active) {
    std::scoped_lock lock(m_mutex);
    m_talking = active;
}

bool VoiceManager::TalkState() const {
    std::scoped_lock lock(m_mutex);
    return m_talking;
}

void VoiceManager::HandleIncoming(const VoiceEnvelope& envelope) {
    if (envelope.type == PacketType::State) {
        debug("VOIP state update from " + std::to_string(envelope.sender_id) + " active=" + std::to_string(int(envelope.voice_active)));
        return;
    }
    if (envelope.type == PacketType::Frame) {
#if defined(BEAMMP_VOIP_HAS_OPUS) && defined(BEAMMP_VOIP_HAS_PORTAUDIO)
        if (!m_backend_running || envelope.payload.empty()) {
            return;
        }
        RuntimeAudioState& rt = Runtime();
        OpusDecoder* decoder = nullptr;
        auto it = rt.decoders.find(envelope.sender_id);
        if (it == rt.decoders.end()) {
            int err = OPUS_OK;
            decoder = opus_decoder_create(kSampleRate, kChannels, &err);
            if (!decoder || err != OPUS_OK) {
                return;
            }
            rt.decoders[envelope.sender_id] = decoder;
        } else {
            decoder = it->second;
        }

        std::vector<opus_int16> pcm(kFrameSamples, 0);
        const int decoded = opus_decode(decoder, envelope.payload.data(), static_cast<opus_int32>(envelope.payload.size()), pcm.data(), kFrameSamples, 0);
        if (decoded <= 0) {
            return;
        }
        std::vector<float> frame(kFrameSamples, 0.0f);
        for (int i = 0; i < std::min(decoded, kFrameSamples); ++i) {
            frame[i] = static_cast<float>(pcm[i]) / 32768.0f;
        }
        {
            std::scoped_lock lock(m_audio_mutex);
            auto& q = m_jitter_pcm[envelope.sender_id];
            q.push_back(std::move(frame));
            while (q.size() > 25) {
                q.pop_front();
            }
        }
        m_audio_cv.notify_one();
#endif
    }
}

std::string VoiceManager::MakeStatePacket(bool active) const {
    std::scoped_lock lock(m_mutex);
    VoiceEnvelope env {};
    env.type = PacketType::State;
    env.sender_id = m_local_player_id;
    env.sequence = m_local_sequence + 1;
    env.voice_active = active;
    env.timestamp_ms = 0;
    return SerializeVoiceEnvelope(env);
}

std::string VoiceManager::MakeHelloPacket() const {
    std::scoped_lock lock(m_mutex);
    VoiceEnvelope env {};
    env.type = PacketType::Hello;
    env.sender_id = m_local_player_id;
    env.sequence = 0;
    env.timestamp_ms = 0;
    return SerializeVoiceEnvelope(env);
}

std::string VoiceManager::MakeByePacket() const {
    std::scoped_lock lock(m_mutex);
    VoiceEnvelope env {};
    env.type = PacketType::Bye;
    env.sender_id = m_local_player_id;
    env.sequence = 0;
    env.timestamp_ms = 0;
    return SerializeVoiceEnvelope(env);
}

void VoiceManager::UpdateSpatialState(const SpatialPacket& packet) {
    SpatialState state {};
    state.x = packet.x;
    state.y = packet.y;
    state.z = packet.z;
    state.vx = packet.vx;
    state.vy = packet.vy;
    state.vz = packet.vz;
    state.updated_at = std::chrono::steady_clock::now();
    UpdateSpatialState(packet.player_id, state);
}

void VoiceManager::UpdateSpatialState(int player_id, const SpatialState& state) {
    std::scoped_lock lock(m_mutex);
    m_spatial[player_id] = state;
}

float VoiceManager::ComputeAttenuation(int remote_player_id) {
    std::scoped_lock lock(m_mutex);
    if (m_local_player_id < 0 || remote_player_id < 0) {
        return 1.0f;
    }
    auto local_it = m_spatial.find(m_local_player_id);
    auto remote_it = m_spatial.find(remote_player_id);
    if (local_it == m_spatial.end() || remote_it == m_spatial.end()) {
        return 1.0f;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - remote_it->second.updated_at).count();
    if (age > 4) {
        return 0.0f;
    }

    const float distance = Distance(local_it->second, remote_it->second);
    const float near_d = std::max(0.0f, m_settings.attenuation_near_meters);
    const float far_d = std::max(near_d + 1.0f, m_settings.attenuation_far_meters);
    float target = 1.0f;
    if (distance >= far_d) {
        target = 0.0f;
    } else if (distance > near_d) {
        const float t = (distance - near_d) / (far_d - near_d);
        target = 1.0f - Clamp01(t);
    }
    auto& smoothed = m_smoothed_gain[remote_player_id];
    smoothed += (target - smoothed) * Clamp01(m_settings.attenuation_smoothing);
    return smoothed * m_settings.output_gain;
}

bool VoiceManager::BackendAvailable() const {
#if defined(BEAMMP_VOIP_HAS_OPUS) && defined(BEAMMP_VOIP_HAS_PORTAUDIO)
    return true;
#else
    return false;
#endif
}

void VoiceManager::CaptureLoop() {
#if defined(BEAMMP_VOIP_HAS_OPUS) && defined(BEAMMP_VOIP_HAS_PORTAUDIO)
    RuntimeAudioState& rt = Runtime();
    std::vector<opus_int16> mic(kFrameSamples, 0);
    std::vector<unsigned char> encoded(kOpusMaxBytes, 0);
    bool last_active = false;

    while (m_backend_running && !Terminate) {
        if (!rt.input_stream || !rt.encoder) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (Pa_ReadStream(rt.input_stream, mic.data(), kFrameSamples) != paNoError) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        VoiceSettings settings = GetSettings();
        bool active = false;
        if (settings.push_to_talk) {
            active = TalkState();
        } else {
            double energy = 0.0;
            for (int i = 0; i < kFrameSamples; ++i) {
                const float s = static_cast<float>(mic[i]) / 32768.0f;
                energy += (s * s);
            }
            const float rms = static_cast<float>(std::sqrt(energy / kFrameSamples));
            active = rms > 0.01f;
        }

        if (active != last_active) {
            last_active = active;
            ServerSend(MakeStatePacket(active), true);
        }

        if (!active || !settings.enabled) {
            continue;
        }
        if (settings.mic_gain != 1.0f) {
            for (int i = 0; i < kFrameSamples; ++i) {
                float v = static_cast<float>(mic[i]) * settings.mic_gain;
                if (v > 32767.0f) {
                    v = 32767.0f;
                } else if (v < -32768.0f) {
                    v = -32768.0f;
                }
                mic[i] = static_cast<opus_int16>(v);
            }
        }
        const int bytes = opus_encode(rt.encoder, mic.data(), kFrameSamples, encoded.data(), kOpusMaxBytes);
        if (bytes <= 0) {
            continue;
        }
        VoiceEnvelope env {};
        env.type = PacketType::Frame;
        {
            std::scoped_lock lock(m_mutex);
            env.sender_id = m_local_player_id;
        }
        env.sequence = ++m_frame_sequence;
        env.timestamp_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        env.voice_active = true;
        env.payload.assign(encoded.begin(), encoded.begin() + bytes);
        ServerSend(SerializeVoiceEnvelope(env), false);
    }
#endif
}

void VoiceManager::PlaybackLoop() {
#if defined(BEAMMP_VOIP_HAS_OPUS) && defined(BEAMMP_VOIP_HAS_PORTAUDIO)
    RuntimeAudioState& rt = Runtime();
    std::vector<float> mixed(kFrameSamples, 0.0f);
    while (m_backend_running && !Terminate) {
        std::unique_lock lk(m_audio_mutex);
        m_audio_cv.wait_for(lk, std::chrono::milliseconds(20));
        std::fill(mixed.begin(), mixed.end(), 0.0f);
        VoiceSettings settings = GetSettings();
        for (auto it = m_jitter_pcm.begin(); it != m_jitter_pcm.end();) {
            const int sender = it->first;
            auto& q = it->second;
            if (!q.empty()) {
                std::vector<float> frame = std::move(q.front());
                q.pop_front();
                lk.unlock();
                const float gain = ComputeAttenuation(sender);
                lk.lock();
                for (int i = 0; i < kFrameSamples && i < static_cast<int>(frame.size()); ++i) {
                    mixed[i] += frame[i] * gain * settings.output_gain;
                }
            }
            if (q.empty()) {
                ++it;
            } else {
                ++it;
            }
        }
        lk.unlock();
        for (float& sample : mixed) {
            if (sample > 1.0f) {
                sample = 1.0f;
            } else if (sample < -1.0f) {
                sample = -1.0f;
            }
        }
        if (rt.output_stream) {
            Pa_WriteStream(rt.output_stream, mixed.data(), kFrameSamples);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
#endif
}

} // namespace Voice
