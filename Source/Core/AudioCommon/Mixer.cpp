// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/Mixer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <fmt/format.h>

#include "AudioCommon/Enums.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/Swap.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/BroadcastSink.h"
#include "Core/Orca/Profile.h"
#include "Core/Rollback/Rollback.h"
#include "Core/System.h"
#include "VideoCommon/PerformanceMetrics.h"

// Orca diag. ORCA_MIXER_WAV=<path> ("%p" becomes the process id): a 16-bit stereo WAV of
// Mixer::Mix's output, after resampling and gap filling, before the backend's own volume or mute.
// The audio thread only appends to a buffer; a writer thread saves it every 250 ms and rewrites
// the header sizes each time, so a killed process still leaves a playable file.
class OrcaMixerWavTap final
{
public:
  explicit OrcaMixerWavTap(std::string path) : m_path(std::move(path)) {}
  ~OrcaMixerWavTap()
  {
    {
      std::lock_guard lock(m_mutex);
      m_stop = true;
    }
    m_wake.notify_one();
    if (m_writer.joinable())
      m_writer.join();
  }
  OrcaMixerWavTap(const OrcaMixerWavTap&) = delete;
  OrcaMixerWavTap& operator=(const OrcaMixerWavTap&) = delete;

  static std::unique_ptr<OrcaMixerWavTap> FromEnv()
  {
    std::string path = Orca::GetEnv("ORCA_MIXER_WAV");
    if (path.empty())
      return nullptr;
#ifdef _WIN32
    const long long pid = _getpid();
#else
    const long long pid = getpid();
#endif
    for (std::size_t at = path.find("%p"); at != std::string::npos; at = path.find("%p", at))
      path.replace(at, 2, std::to_string(pid));
    // A second Mixer in the same process (another boot) gets its own file.
    static std::atomic<int> s_orca_wav_count{0};
    const int index = s_orca_wav_count.fetch_add(1);
    if (index > 0)
      path += fmt::format(".{}.wav", index + 1);
    return std::make_unique<OrcaMixerWavTap>(std::move(path));
  }

  // Audio thread. Host-endian interleaved left/right, as Mix writes them.
  void Add(const s16* samples, std::size_t num_frames, u32 sample_rate)
  {
    if (m_failed || sample_rate == 0)
      return;
    if (!m_started)
    {
      m_started = true;
      m_sample_rate = sample_rate;
      m_writer = std::thread([this] { WriterLoop(); });
    }
    if (sample_rate != m_sample_rate)
      return;
    std::lock_guard lock(m_mutex);
    // Ten seconds behind means the disk stalled; drop rather than grow without bound.
    if (m_pending.size() + num_frames * 2 > std::size_t(m_sample_rate) * 2 * 10)
    {
      m_lost_frames += num_frames;
      return;
    }
    m_pending.insert(m_pending.end(), samples, samples + num_frames * 2);
  }

private:
  void WriterLoop()
  {
    File::IOFile file(m_path, "wb");
    if (!file)
    {
      ERROR_LOG_FMT(ROLLBACK, "Orca mixer: ORCA_MIXER_WAV couldn't open {}", m_path);
      m_failed = true;
      return;
    }
    NOTICE_LOG_FMT(ROLLBACK, "Orca mixer: writing the mixer output ({} Hz stereo s16) to {}",
                   m_sample_rate, m_path);
    u64 data_bytes = 0;
    WriteHeader(file, 0);
    std::vector<s16> chunk;
    bool stop = false;
    u64 lost_logged = 0;
    while (!stop)
    {
      {
        std::unique_lock lock(m_mutex);
        m_wake.wait_for(lock, std::chrono::milliseconds(250), [this] { return m_stop; });
        stop = m_stop;
        chunk.swap(m_pending);
      }
      if (!chunk.empty())
      {
        // WAV data is little-endian.
        if constexpr (std::endian::native == std::endian::big)
        {
          for (s16& sample : chunk)
            sample = static_cast<s16>(Common::swap16(static_cast<u16>(sample)));
        }
        file.WriteArray(chunk.data(), chunk.size());
        data_bytes += chunk.size() * sizeof(s16);
        chunk.clear();
        WriteHeader(file, data_bytes);
        file.Seek(0, File::SeekOrigin::End);
        file.Flush();
      }
      std::size_t lost;
      {
        std::lock_guard lock(m_mutex);
        lost = m_lost_frames;
      }
      if (lost != lost_logged)
      {
        WARN_LOG_FMT(ROLLBACK, "Orca mixer: the WAV tap fell behind and lost {} frames", lost);
        lost_logged = lost;
      }
    }
  }

  void WriteHeader(File::IOFile& file, u64 data_bytes)
  {
    const u32 data_size = static_cast<u32>(std::min<u64>(data_bytes, 0xFFFFFFFFull - 36));
    const auto u32le = [](u8* out, u32 value) {
      for (int i = 0; i < 4; ++i)
        out[i] = static_cast<u8>(value >> (8 * i));
    };
    std::array<u8, 44> header{};
    std::memcpy(&header[0], "RIFF", 4);
    u32le(&header[4], 36 + data_size);
    std::memcpy(&header[8], "WAVEfmt ", 8);
    u32le(&header[16], 16);
    header[20] = 1;  // PCM
    header[22] = 2;  // channels
    u32le(&header[24], m_sample_rate);
    u32le(&header[28], m_sample_rate * 4);
    header[32] = 4;   // block align
    header[34] = 16;  // bits per sample
    std::memcpy(&header[36], "data", 4);
    u32le(&header[40], data_size);
    file.Seek(0, File::SeekOrigin::Begin);
    file.WriteArray(header.data(), header.size());
  }

  const std::string m_path;
  // Audio thread only, apart from m_sample_rate, which is set before the writer starts.
  bool m_started = false;
  u32 m_sample_rate = 0;
  std::atomic<bool> m_failed{false};
  std::thread m_writer;

  std::mutex m_mutex;
  std::condition_variable m_wake;
  std::vector<s16> m_pending;
  std::size_t m_lost_frames = 0;
  bool m_stop = false;
};

static u32 DPL2QualityToFrameBlockSize(AudioCommon::DPL2Quality quality)
{
  switch (quality)
  {
  case AudioCommon::DPL2Quality::Lowest:
    return 512;
  case AudioCommon::DPL2Quality::Low:
    return 1024;
  case AudioCommon::DPL2Quality::Highest:
    return 4096;
  default:
    return 2048;
  }
}

Mixer::Mixer(u32 BackendSampleRate)
    : m_output_sample_rate(BackendSampleRate),
      m_surround_decoder(BackendSampleRate,
                         DPL2QualityToFrameBlockSize(Config::Get(Config::MAIN_DPL2_QUALITY)))
{
  m_config_changed_callback_id = Config::AddConfigChangedCallback([this] { RefreshConfig(); });
  RefreshConfig();
  m_orca_wav_tap = OrcaMixerWavTap::FromEnv();

  INFO_LOG_FMT(AUDIO_INTERFACE, "Mixer is initialized");
}

Mixer::~Mixer()
{
  Config::RemoveConfigChangedCallback(m_config_changed_callback_id);
}

void Mixer::DoState(PointerWrap& p)
{
  m_dma_mixer.DoState(p);
  m_streaming_mixer.DoState(p);
  for (auto& mixer : m_wiimote_speaker_mixers)
    mixer.DoState(p);
  m_skylander_portal_mixer.DoState(p);
  for (auto& mixer : m_gba_mixers)
    mixer.DoState(p);
}

// Executed from sound stream thread
void Mixer::MixerFifo::Mix(s16* samples, std::size_t num_samples)
{
  constexpr u32 INDEX_HALF = 0x80000000;
  constexpr DT_s FADE_IN_RC = DT_s(0.008);
  constexpr DT_s FADE_OUT_RC = DT_s(0.064);

  // We need at least a double because the index jump has 24 bits of fractional precision.
  const double out_sample_rate = m_mixer->m_output_sample_rate;
  double in_sample_rate = double(m_input_sample_rate_dividend) / m_input_sample_rate_divisor;

  const double emulation_speed = m_mixer->m_config_emulation_speed;
  if (!m_mixer->m_config_audio_preserve_pitch && 0 < emulation_speed && emulation_speed != 1.0)
    in_sample_rate *= emulation_speed;

  const double base = static_cast<double>(1 << GRANULE_FRAC_BITS);
  const u32 index_jump = std::lround(base * in_sample_rate / out_sample_rate);

  // These fade in / out multiplier are tuned to match a constant
  // fade speed regardless of the input or the output sample rate.
  const float fade_in_mul = -std::expm1(-DT_s(1.0) / (out_sample_rate * FADE_IN_RC));
  const float fade_out_mul = -std::expm1(-DT_s(1.0) / (out_sample_rate * FADE_OUT_RC));

  const StereoPair volume{m_LVolume.load() / 256.0f, m_RVolume.load() / 256.0f};

  // Calculate the ideal length of the granule queue.
  const std::size_t buffer_size_ms = m_mixer->m_config_audio_buffer_ms;
  const std::size_t buffer_size_samples = std::llround(buffer_size_ms * in_sample_rate / 1000.0);

  // Limit the possible queue sizes to any number between 4 and 64.
  const std::size_t buffer_size_granules =
      std::clamp((buffer_size_samples) / (GRANULE_SIZE >> 1), static_cast<std::size_t>(4),
                 static_cast<std::size_t>(MAX_GRANULE_QUEUE_SIZE));

  bool fade_audio = m_queue_fading.load(std::memory_order_relaxed);

  m_granule_queue_size.store(buffer_size_granules, std::memory_order_relaxed);

  while (num_samples-- > 0)
  {
    // The indexes for the front and back buffers are offset by 50% of the granule size.
    // We use the modular nature of 32-bit integers to wrap around the granule size.
    m_current_index += index_jump;
    const u32 front_index = m_current_index;
    const u32 back_index = m_current_index + INDEX_HALF;

    // If either index is less than the index jump, that means we reached
    // the end of the of the buffer and need to load the next granule.
    if (front_index < index_jump)
      fade_audio = Dequeue(&m_front);
    else if (back_index < index_jump)
      fade_audio = Dequeue(&m_back);

    // The Granules are pre-windowed, so we can just add them together
    const std::size_t ft = front_index >> GRANULE_FRAC_BITS;
    const std::size_t bt = back_index >> GRANULE_FRAC_BITS;
    const StereoPair s0 = m_front[(ft - 2) & GRANULE_MASK] + m_back[(bt - 2) & GRANULE_MASK];
    const StereoPair s1 = m_front[(ft - 1) & GRANULE_MASK] + m_back[(bt - 1) & GRANULE_MASK];
    const StereoPair s2 = m_front[(ft + 0) & GRANULE_MASK] + m_back[(bt + 0) & GRANULE_MASK];
    const StereoPair s3 = m_front[(ft + 1) & GRANULE_MASK] + m_back[(bt + 1) & GRANULE_MASK];
    const StereoPair s4 = m_front[(ft + 2) & GRANULE_MASK] + m_back[(bt + 2) & GRANULE_MASK];
    const StereoPair s5 = m_front[(ft + 3) & GRANULE_MASK] + m_back[(bt + 3) & GRANULE_MASK];

    // Polynomial Interpolators for High-Quality Resampling of
    // Over Sampled Audio by Olli Niemitalo, October 2001.
    // Page 43 -- 6-point, 3rd-order Hermite:
    // https://yehar.com/blog/wp-content/uploads/2009/08/deip.pdf
    const u32 t_frac = m_current_index & ((1 << GRANULE_FRAC_BITS) - 1);
    const float t1 = t_frac / static_cast<float>(1 << GRANULE_FRAC_BITS);
    const float t2 = t1 * t1;
    const float t3 = t2 * t1;

    StereoPair sample = (s0 * StereoPair{(+0.0f + 1.0f * t1 - 2.0f * t2 + 1.0f * t3) / 12.0f} +
                         s1 * StereoPair{(+0.0f - 8.0f * t1 + 15.0f * t2 - 7.0f * t3) / 12.0f} +
                         s2 * StereoPair{(+3.0f + 0.0f * t1 - 7.0f * t2 + 4.0f * t3) / 3.0f} +
                         s3 * StereoPair{(+0.0f + 2.0f * t1 + 5.0f * t2 - 4.0f * t3) / 3.0f} +
                         s4 * StereoPair{(+0.0f - 1.0f * t1 - 6.0f * t2 + 7.0f * t3) / 12.0f} +
                         s5 * StereoPair{(+0.0f + 0.0f * t1 + 1.0f * t2 - 1.0f * t3) / 12.0f});

    // Apply Fade In / Fade Out depending on if we are looping
    if (fade_audio)
      m_fade_volume += fade_out_mul * (0.0f - m_fade_volume);
    else
      m_fade_volume += fade_in_mul * (1.0f - m_fade_volume);

    // Apply the fade volume and the regular volume to the sample
    sample = sample * volume * StereoPair{m_fade_volume};

    // This quantization method prevents accumulated error but does not do noise shaping.
    sample.l += samples[0] - m_quantization_error.l;
    samples[0] = MathUtil::SaturatingCast<s16>(std::lround(sample.l));
    m_quantization_error.l = std::clamp(samples[0] - sample.l, -1.0f, 1.0f);

    sample.r += samples[1] - m_quantization_error.r;
    samples[1] = MathUtil::SaturatingCast<s16>(std::lround(sample.r));
    m_quantization_error.r = std::clamp(samples[1] - sample.r, -1.0f, 1.0f);

    samples += 2;
  }
}

std::size_t Mixer::Mix(s16* samples, std::size_t num_samples)
{
  if (!samples)
    return 0;

  memset(samples, 0, num_samples * 2 * sizeof(s16));

  m_dma_mixer.Mix(samples, num_samples);
  m_streaming_mixer.Mix(samples, num_samples);
  for (std::size_t i = 0; i < m_wiimote_speaker_mixers.size(); ++i)
  {
    if (!m_config_wiimote_routing_enabled || !m_config_wiimote_output_enabled[i])
      m_wiimote_speaker_mixers[i].Mix(samples, num_samples);
  }
  m_skylander_portal_mixer.Mix(samples, num_samples);
  for (auto& mixer : m_gba_mixers)
    mixer.Mix(samples, num_samples);

  if (Broadcast::Active())
    Broadcast::Audio(samples, num_samples, m_output_sample_rate);

  m_orca_out_frames.fetch_add(num_samples, std::memory_order_relaxed);
  if (m_orca_wav_tap)
    m_orca_wav_tap->Add(samples, num_samples, m_output_sample_rate);

  return num_samples;
}

std::size_t Mixer::MixSurround(float* samples, std::size_t num_samples)
{
  if (!num_samples)
    return 0;

  memset(samples, 0, num_samples * SURROUND_CHANNELS * sizeof(float));

  const std::size_t needed_frames =
      m_surround_decoder.QueryFramesNeededForSurroundOutput(num_samples);

  constexpr std::size_t max_samples = 0x8000;
  ASSERT_MSG(AUDIO, needed_frames <= max_samples,
             "needed_frames would overflow m_scratch_buffer: {} -> {} > {}", num_samples,
             needed_frames, max_samples);

  std::array<s16, max_samples> buffer;
  const std::size_t available_frames = Mix(buffer.data(), static_cast<std::size_t>(needed_frames));
  if (available_frames != needed_frames)
  {
    ERROR_LOG_FMT(AUDIO,
                  "Error decoding surround frames: needed {} frames for {} samples but got {}",
                  needed_frames, num_samples, available_frames);
    return 0;
  }

  m_surround_decoder.PutFrames(buffer.data(), needed_frames);
  m_surround_decoder.ReceiveFrames(samples, num_samples);

  return num_samples;
}

void Mixer::PushSamples(const s16* samples, std::size_t num_samples)
{
  OrcaMaybeLogStats();
  // Orca rollback: frames re-run after a load already played their sound the first time.
  if (Rollback::IsResimulating())
  {
    m_dma_mixer.OrcaCountResimDropped(num_samples);
    return;
  }
  m_dma_mixer.OrcaCountPushed(num_samples);
  if (IsOutputSampleRateValid())
  {
    // Big-endian RL-orderered stereo samples.

    const s16* ptr = samples;
    for (std::size_t i = 0; i != num_samples; ++i)
    {
      m_dma_mixer.PushSample(Common::swap16(ptr[1]), Common::swap16(ptr[0]));
      ptr += 2;
    }
  }

  if (m_log_dsp_audio)
  {
    const s32 sample_rate_divisor = m_dma_mixer.GetInputSampleRateDivisor();
    auto const volume = m_dma_mixer.GetVolume();
    m_wave_writer_dsp.AddStereoSamplesBE(samples, static_cast<u32>(num_samples),
                                         sample_rate_divisor, volume.first, volume.second);
  }
}

void Mixer::PushStreamingSamples(const s16* samples, std::size_t num_samples)
{
  if (Rollback::IsResimulating())
  {
    m_streaming_mixer.OrcaCountResimDropped(num_samples);
    return;
  }
  m_streaming_mixer.OrcaCountPushed(num_samples);
  if (IsOutputSampleRateValid())
  {
    // Big-endian RL-orderered stereo samples.

    const s16* ptr = samples;
    for (std::size_t i = 0; i != num_samples; ++i)
    {
      m_streaming_mixer.PushSample(Common::swap16(ptr[1]), Common::swap16(ptr[0]));
      ptr += 2;
    }
  }

  if (m_log_dtk_audio)
  {
    const s32 sample_rate_divisor = m_streaming_mixer.GetInputSampleRateDivisor();
    const auto volume = m_streaming_mixer.GetVolume();
    m_wave_writer_dtk.AddStereoSamplesBE(samples, static_cast<u32>(num_samples),
                                         sample_rate_divisor, volume.first, volume.second);
  }
}

void Mixer::PushWiimoteSpeakerSamples(std::size_t wiimote_index, const s16* samples,
                                      std::size_t num_samples, u32 sample_rate_divisor)
{
  if (wiimote_index >= m_wiimote_speaker_mixers.size())
    return;
  if (Rollback::IsResimulating())
  {
    m_wiimote_speaker_mixers[wiimote_index].OrcaCountResimDropped(num_samples);
    return;
  }
  m_wiimote_speaker_mixers[wiimote_index].OrcaCountPushed(num_samples);
  if (!IsOutputSampleRateValid())
    return;

  // WiimoteEmu produces host-endian mono samples.

  m_wiimote_speaker_mixers[wiimote_index].SetInputSampleRateDivisor(sample_rate_divisor);

  for (const s16 sample : std::span{samples, num_samples})
  {
    m_wiimote_speaker_mixers[wiimote_index].PushSample(sample, sample);
  }
}

std::size_t Mixer::MixWiimoteSpeaker(std::size_t wiimote_index, s16* samples,
                                     std::size_t num_samples)
{
  if (!samples || wiimote_index >= m_wiimote_speaker_mixers.size())
    return 0;

  memset(samples, 0, num_samples * 2 * sizeof(s16));
  m_wiimote_speaker_mixers[wiimote_index].Mix(samples, num_samples);
  return num_samples;
}

void Mixer::PushSkylanderPortalSamples(const u8* samples, std::size_t num_samples)
{
  if (!IsOutputSampleRateValid())
    return;

  // Skylander samples are always supplied as 64 bytes, 32 x 16 bit samples
  // The portal speaker is 1 channel, so duplicate and play as stereo audio

  while (num_samples--)
  {
    // Little-endian data.
    const s16 sample = u16(samples[0] | u16(samples[1] << 8u));
    m_skylander_portal_mixer.PushSample(sample, sample);
    samples += 2;
  }
}

void Mixer::PushGBASamples(std::size_t device_number, const s16* samples, std::size_t num_samples)
{
  if (!IsOutputSampleRateValid())
    return;

  // Integrated GBA pushes host-endian LR-ordered stereo samples.

  while (num_samples--)
  {
    m_gba_mixers[device_number].PushSample(samples[0], samples[1]);
    samples += 2;
  }
}

void Mixer::SetDMAInputSampleRateDivisor(u32 rate_divisor)
{
  m_dma_mixer.SetInputSampleRateDivisor(rate_divisor);
}

void Mixer::SetStreamInputSampleRateDivisor(u32 rate_divisor)
{
  m_streaming_mixer.SetInputSampleRateDivisor(rate_divisor);
}

void Mixer::SetGBAInputSampleRate(std::size_t device_number, u32 sample_rate)
{
  m_gba_mixers[device_number].SetInputSampleRateDivisor(GBA_SAMPLE_RATE_DIVIDEND / sample_rate);
}

void Mixer::SetStreamingVolume(u32 lvolume, u32 rvolume)
{
  m_streaming_mixer.SetVolume(std::clamp<u32>(lvolume, 0x00, 0xff),
                              std::clamp<u32>(rvolume, 0x00, 0xff));
}

void Mixer::SetWiimoteSpeakerVolume(std::size_t wiimote_index, u32 lvolume, u32 rvolume)
{
  if (wiimote_index < m_wiimote_speaker_mixers.size())
    m_wiimote_speaker_mixers[wiimote_index].SetVolume(lvolume, rvolume);
}

void Mixer::SetGBAVolume(std::size_t device_number, u32 lvolume, u32 rvolume)
{
  m_gba_mixers[device_number].SetVolume(lvolume, rvolume);
}

void Mixer::StartLogDTKAudio(const std::string& filename)
{
  if (!m_log_dtk_audio)
  {
    const bool success =
        m_wave_writer_dtk.Start(filename, m_streaming_mixer.GetInputSampleRateDivisor());
    if (success)
    {
      m_log_dtk_audio = true;
      m_wave_writer_dtk.SetSkipSilence(false);
      NOTICE_LOG_FMT(AUDIO, "Starting DTK Audio logging");
    }
    else
    {
      m_wave_writer_dtk.Stop();
      NOTICE_LOG_FMT(AUDIO, "Unable to start DTK Audio logging");
    }
  }
  else
  {
    WARN_LOG_FMT(AUDIO, "DTK Audio logging has already been started");
  }
}

void Mixer::StopLogDTKAudio()
{
  if (m_log_dtk_audio)
  {
    m_log_dtk_audio = false;
    m_wave_writer_dtk.Stop();
    NOTICE_LOG_FMT(AUDIO, "Stopping DTK Audio logging");
  }
  else
  {
    WARN_LOG_FMT(AUDIO, "DTK Audio logging has already been stopped");
  }
}

void Mixer::StartLogDSPAudio(const std::string& filename)
{
  if (!m_log_dsp_audio)
  {
    const bool success = m_wave_writer_dsp.Start(filename, m_dma_mixer.GetInputSampleRateDivisor());
    if (success)
    {
      m_log_dsp_audio = true;
      m_wave_writer_dsp.SetSkipSilence(false);
      NOTICE_LOG_FMT(AUDIO, "Starting DSP Audio logging");
    }
    else
    {
      m_wave_writer_dsp.Stop();
      NOTICE_LOG_FMT(AUDIO, "Unable to start DSP Audio logging");
    }
  }
  else
  {
    WARN_LOG_FMT(AUDIO, "DSP Audio logging has already been started");
  }
}

void Mixer::StopLogDSPAudio()
{
  if (m_log_dsp_audio)
  {
    m_log_dsp_audio = false;
    m_wave_writer_dsp.Stop();
    NOTICE_LOG_FMT(AUDIO, "Stopping DSP Audio logging");
  }
  else
  {
    WARN_LOG_FMT(AUDIO, "DSP Audio logging has already been stopped");
  }
}

void Mixer::RefreshConfig()
{
  m_config_emulation_speed = Config::Get(Config::MAIN_EMULATION_SPEED);
  m_config_audio_preserve_pitch = Config::Get(Config::MAIN_AUDIO_PRESERVE_PITCH);
  m_config_fill_audio_gaps = Config::Get(Config::MAIN_AUDIO_FILL_GAPS);
  m_config_audio_buffer_ms = Config::Get(Config::MAIN_AUDIO_BUFFER_SIZE);
  m_config_wiimote_routing_enabled = Config::Get(Config::MAIN_WIIMOTE_AUDIO_ROUTING_ENABLED);
  for (std::size_t i = 0; i < m_config_wiimote_output_enabled.size(); ++i)
    m_config_wiimote_output_enabled[i] = Config::Get(Config::MAIN_WIIMOTE_AUDIO_OUTPUT_ENABLED[i]);
}

void Mixer::MixerFifo::DoState(PointerWrap& p)
{
  p.Do(m_input_sample_rate_divisor);
  p.Do(m_LVolume);
  p.Do(m_RVolume);
}

void Mixer::MixerFifo::SetInputSampleRateDividend(u32 rate_dividend)
{
  m_input_sample_rate_dividend = rate_dividend;
}

u32 Mixer::MixerFifo::GetInputSampleRateDividend() const
{
  return m_input_sample_rate_dividend;
}

void Mixer::MixerFifo::SetInputSampleRateDivisor(u32 rate_divisor)
{
  m_input_sample_rate_divisor = rate_divisor;
}

u32 Mixer::MixerFifo::GetInputSampleRateDivisor() const
{
  return m_input_sample_rate_divisor;
}

void Mixer::MixerFifo::SetVolume(u32 lvolume, u32 rvolume)
{
  m_LVolume.store(lvolume + (lvolume >> 7));
  m_RVolume.store(rvolume + (rvolume >> 7));
}

std::pair<s32, s32> Mixer::MixerFifo::GetVolume() const
{
  return std::make_pair(m_LVolume.load(), m_RVolume.load());
}

void Mixer::MixerFifo::Enqueue()
{
  // import numpy as np
  // import scipy.signal as signal
  // window = np.convolve(np.ones(128), signal.windows.dpss(128 + 1, 4))
  // window /= (window[:len(window) // 2] + window[len(window) // 2:]).max()
  // elements = ", ".join([f"{x:.10f}f" for x in window])
  // print(f'constexpr std::array<StereoPair, GRANULE_SIZE> GRANULE_WINDOW = {{ {elements}
  // }};')
  static constexpr std::array<StereoPair, GRANULE_SIZE> GRANULE_WINDOW = {
      0.0000016272f, 0.0000050749f, 0.0000113187f, 0.0000216492f, 0.0000377350f, 0.0000616906f,
      0.0000961509f, 0.0001443499f, 0.0002102045f, 0.0002984010f, 0.0004144844f, 0.0005649486f,
      0.0007573262f, 0.0010002765f, 0.0013036694f, 0.0016786636f, 0.0021377783f, 0.0026949534f,
      0.0033656000f, 0.0041666352f, 0.0051165029f, 0.0062351752f, 0.0075441359f, 0.0090663409f,
      0.0108261579f, 0.0128492811f, 0.0151626215f, 0.0177941726f, 0.0207728499f, 0.0241283062f,
      0.0278907219f, 0.0320905724f, 0.0367583739f, 0.0419244083f, 0.0476184323f, 0.0538693708f,
      0.0607049996f, 0.0681516192f, 0.0762337261f, 0.0849736833f, 0.0943913952f, 0.1045039915f,
      0.1153255250f, 0.1268666867f, 0.1391345431f, 0.1521323012f, 0.1658591025f, 0.1803098534f,
      0.1954750915f, 0.2113408944f, 0.2278888303f, 0.2450959552f, 0.2629348550f, 0.2813737361f,
      0.3003765625f, 0.3199032396f, 0.3399098438f, 0.3603488941f, 0.3811696664f, 0.4023185434f,
      0.4237393998f, 0.4453740162f, 0.4671625177f, 0.4890438330f, 0.5109561670f, 0.5328374823f,
      0.5546259838f, 0.5762606002f, 0.5976814566f, 0.6188303336f, 0.6396511059f, 0.6600901562f,
      0.6800967604f, 0.6996234375f, 0.7186262639f, 0.7370651450f, 0.7549040448f, 0.7721111697f,
      0.7886591056f, 0.8045249085f, 0.8196901466f, 0.8341408975f, 0.8478676988f, 0.8608654569f,
      0.8731333133f, 0.8846744750f, 0.8954960085f, 0.9056086048f, 0.9150263167f, 0.9237662739f,
      0.9318483808f, 0.9392950004f, 0.9461306292f, 0.9523815677f, 0.9580755917f, 0.9632416261f,
      0.9679094276f, 0.9721092781f, 0.9758716938f, 0.9792271501f, 0.9822058274f, 0.9848373785f,
      0.9871507189f, 0.9891738421f, 0.9909336591f, 0.9924558641f, 0.9937648248f, 0.9948834971f,
      0.9958333648f, 0.9966344000f, 0.9973050466f, 0.9978622217f, 0.9983213364f, 0.9986963306f,
      0.9989997235f, 0.9992426738f, 0.9994350514f, 0.9995855156f, 0.9997015990f, 0.9997897955f,
      0.9998556501f, 0.9999038491f, 0.9999383094f, 0.9999622650f, 0.9999783508f, 0.9999886813f,
      0.9999949251f, 0.9999983728f, 0.9999983728f, 0.9999949251f, 0.9999886813f, 0.9999783508f,
      0.9999622650f, 0.9999383094f, 0.9999038491f, 0.9998556501f, 0.9997897955f, 0.9997015990f,
      0.9995855156f, 0.9994350514f, 0.9992426738f, 0.9989997235f, 0.9986963306f, 0.9983213364f,
      0.9978622217f, 0.9973050466f, 0.9966344000f, 0.9958333648f, 0.9948834971f, 0.9937648248f,
      0.9924558641f, 0.9909336591f, 0.9891738421f, 0.9871507189f, 0.9848373785f, 0.9822058274f,
      0.9792271501f, 0.9758716938f, 0.9721092781f, 0.9679094276f, 0.9632416261f, 0.9580755917f,
      0.9523815677f, 0.9461306292f, 0.9392950004f, 0.9318483808f, 0.9237662739f, 0.9150263167f,
      0.9056086048f, 0.8954960085f, 0.8846744750f, 0.8731333133f, 0.8608654569f, 0.8478676988f,
      0.8341408975f, 0.8196901466f, 0.8045249085f, 0.7886591056f, 0.7721111697f, 0.7549040448f,
      0.7370651450f, 0.7186262639f, 0.6996234375f, 0.6800967604f, 0.6600901562f, 0.6396511059f,
      0.6188303336f, 0.5976814566f, 0.5762606002f, 0.5546259838f, 0.5328374823f, 0.5109561670f,
      0.4890438330f, 0.4671625177f, 0.4453740162f, 0.4237393998f, 0.4023185434f, 0.3811696664f,
      0.3603488941f, 0.3399098438f, 0.3199032396f, 0.3003765625f, 0.2813737361f, 0.2629348550f,
      0.2450959552f, 0.2278888303f, 0.2113408944f, 0.1954750915f, 0.1803098534f, 0.1658591025f,
      0.1521323012f, 0.1391345431f, 0.1268666867f, 0.1153255250f, 0.1045039915f, 0.0943913952f,
      0.0849736833f, 0.0762337261f, 0.0681516192f, 0.0607049996f, 0.0538693708f, 0.0476184323f,
      0.0419244083f, 0.0367583739f, 0.0320905724f, 0.0278907219f, 0.0241283062f, 0.0207728499f,
      0.0177941726f, 0.0151626215f, 0.0128492811f, 0.0108261579f, 0.0090663409f, 0.0075441359f,
      0.0062351752f, 0.0051165029f, 0.0041666352f, 0.0033656000f, 0.0026949534f, 0.0021377783f,
      0.0016786636f, 0.0013036694f, 0.0010002765f, 0.0007573262f, 0.0005649486f, 0.0004144844f,
      0.0002984010f, 0.0002102045f, 0.0001443499f, 0.0000961509f, 0.0000616906f, 0.0000377350f,
      0.0000216492f, 0.0000113187f, 0.0000050749f, 0.0000016272f};

  const std::size_t head = m_queue_head.load(std::memory_order_acquire);

  // Check if we run out of space in the circular queue. (rare)
  const std::size_t next_head = (head + 1) & GRANULE_QUEUE_MASK;
  if (next_head == m_queue_tail.load(std::memory_order_acquire))
  {
    m_orca_full_drops.fetch_add(1, std::memory_order_relaxed);
    WARN_LOG_FMT(AUDIO,
                 "Granule Queue has completely filled and audio samples are being dropped. "
                 "This should not happen unless the audio backend has stopped requesting audio.");
    return;
  }

  // By preconstructing the granule window, we have the best chance of
  // the compiler optimizing this loop using SIMD instructions.
  const std::size_t start_index = m_next_buffer_index;
  for (std::size_t i = 0; i < GRANULE_SIZE; ++i)
    m_queue[head][i] = m_next_buffer[(i + start_index) & GRANULE_MASK] * GRANULE_WINDOW[i];

  m_queue_head.store(next_head, std::memory_order_release);
  m_queue_fading.store(false, std::memory_order_relaxed);
  m_queue_looping.store(false, std::memory_order_relaxed);
}

bool Mixer::MixerFifo::Dequeue(Granule* granule)
{
  const std::size_t granule_queue_size = m_granule_queue_size.load(std::memory_order_relaxed);
  const std::size_t head = m_queue_head.load(std::memory_order_acquire);
  std::size_t tail = m_queue_tail.load(std::memory_order_acquire);

  // Orca diag: the queue's length in granules before this call, and its low point.
  const std::size_t orca_fill = (head - tail) & GRANULE_QUEUE_MASK;
  if (orca_fill < m_orca_min_fill.load(std::memory_order_relaxed))
    m_orca_min_fill.store(orca_fill, std::memory_order_relaxed);
  const bool orca_counting = m_orca_ever_pushed.load(std::memory_order_relaxed);

  // Checks to see if the queue has gotten too long.
  if (granule_queue_size < ((head - tail) & GRANULE_QUEUE_MASK))
  {
    // Jump the playhead to half the queue size behind the head.
    const std::size_t gap = (granule_queue_size >> 1) + 1;
    tail = (head - gap) & GRANULE_QUEUE_MASK;
    m_orca_skips.fetch_add(1, std::memory_order_relaxed);
    m_orca_dropped.fetch_add((orca_fill - gap) * GRANULE_OVERLAP, std::memory_order_relaxed);
  }

  // Checks to see if the queue is empty.
  std::size_t next_tail = (tail + 1) & GRANULE_QUEUE_MASK;
  if (next_tail == head)
  {
    // Only fill gaps when running to prevent stutter on pause.
    const bool is_running = Core::GetState(Core::System::GetInstance()) == Core::State::Running;
    if (orca_counting && is_running)
      m_orca_underruns.fetch_add(1, std::memory_order_relaxed);
    if (m_mixer->m_config_fill_audio_gaps && is_running)
    {
      // Jump the playhead to half the queue size behind the head.
      // This provides smoother audio playback than suddenly stopping.
      const std::size_t gap = std::max<std::size_t>(2, granule_queue_size >> 1) - 1;
      next_tail = (head - gap) & GRANULE_QUEUE_MASK;
      // Orca diag: the granules from head - gap up to the head play again.
      if (orca_counting)
        m_orca_repeated.fetch_add(gap * GRANULE_OVERLAP, std::memory_order_relaxed);

      // Orca: while frames re-run after a load (a rollback or a drop-in catch-up), their sound is
      // dropped on purpose, so the queue stays empty until the re-run ends. Looping the last
      // granules at full volume for that long is heard as the music stuttering; fade them out at
      // once instead, and let the next real granule fade back in.
      bool looping = m_queue_looping.load(std::memory_order_relaxed);
      m_queue_fading.store(looping || Rollback::IsResimulating(), std::memory_order_relaxed);
      m_queue_looping.store(true, std::memory_order_relaxed);
    }
    else
    {
      std::fill(granule->begin(), granule->end(), StereoPair{0.0f, 0.0f});
      m_queue_fading.store(false, std::memory_order_relaxed);
      m_queue_looping.store(false, std::memory_order_relaxed);
      return false;
    }
  }

  *granule = m_queue[tail];
  m_queue_tail.store(next_tail, std::memory_order_release);

  return m_queue_fading.load(std::memory_order_relaxed);
}

Mixer::MixerFifo::OrcaFifoStats Mixer::MixerFifo::OrcaTakeStats()
{
  OrcaFifoStats stats;
  stats.underruns = m_orca_underruns.exchange(0, std::memory_order_relaxed);
  stats.repeated_samples = m_orca_repeated.exchange(0, std::memory_order_relaxed);
  stats.skips = m_orca_skips.exchange(0, std::memory_order_relaxed);
  stats.dropped_samples = m_orca_dropped.exchange(0, std::memory_order_relaxed);
  stats.full_drops = m_orca_full_drops.exchange(0, std::memory_order_relaxed);
  stats.pushed_samples = m_orca_pushed.exchange(0, std::memory_order_relaxed);
  stats.resim_dropped_samples = m_orca_resim_dropped.exchange(0, std::memory_order_relaxed);
  stats.min_fill_granules = m_orca_min_fill.exchange(ORCA_NO_FILL, std::memory_order_relaxed);
  stats.fill_granules = (m_queue_head.load(std::memory_order_acquire) -
                         m_queue_tail.load(std::memory_order_acquire)) &
                        GRANULE_QUEUE_MASK;
  stats.target_granules = m_granule_queue_size.load(std::memory_order_relaxed);
  return stats;
}

namespace
{
// One stream's part of the "Orca mixer:" line. Fill is in granules (each advances
// ORCA_GRANULE_HOP input samples) and in milliseconds at the stream's input rate.
std::string OrcaFormatFifoStats(const char* name, double window_s, double input_rate,
                                std::size_t granule_hop, u64 underruns, u64 repeated, u64 skips,
                                u64 dropped, u64 full_drops, u64 pushed, u64 resim_dropped,
                                std::size_t fill, std::size_t min_fill, std::size_t target,
                                std::size_t no_fill)
{
  const auto to_ms = [&](std::size_t granules) {
    return input_rate > 0 ? granules * granule_hop * 1000.0 / input_rate : 0.0;
  };
  const std::string min_text =
      min_fill == no_fill ? std::string("-") :
                            fmt::format("{}({:.0f}ms)", min_fill, to_ms(min_fill));
  return fmt::format("{}: in_hz={:.0f} fill={}({:.0f}ms) min={} target={}({:.0f}ms) under={} "
                     "repeat_samples={} skip={} drop_samples={} full={} resim_drop_samples={}",
                     name, window_s > 0 ? pushed / window_s : 0.0, fill, to_ms(fill), min_text,
                     target, to_ms(target), underruns, repeated, skips, dropped, full_drops,
                     resim_dropped);
}
}  // namespace

void Mixer::OrcaMaybeLogStats()
{
  // PushSamples runs about 4000 times a second; look at the clock every 256th call.
  if ((++m_orca_push_calls & 0xFF) != 0)
    return;
  const auto now = std::chrono::steady_clock::now();
  if (!m_orca_log_started)
  {
    m_orca_log_started = true;
    m_orca_log_start = now;
    m_orca_window_start = now;
    return;
  }
  if (now - m_orca_window_start < std::chrono::seconds(30))
    return;
  const double window_s = std::chrono::duration<double>(now - m_orca_window_start).count();
  const double since_s = std::chrono::duration<double>(now - m_orca_log_start).count();
  m_orca_window_start = now;

  const auto rate_of = [](const MixerFifo& fifo) {
    const u32 divisor = fifo.GetInputSampleRateDivisor();
    return divisor ? double(fifo.GetInputSampleRateDividend()) / divisor : 0.0;
  };
  const auto part = [&](const char* name, MixerFifo& fifo) {
    const MixerFifo::OrcaFifoStats s = fifo.OrcaTakeStats();
    return OrcaFormatFifoStats(name, window_s, rate_of(fifo), MixerFifo::ORCA_GRANULE_HOP,
                               s.underruns, s.repeated_samples, s.skips, s.dropped_samples,
                               s.full_drops, s.pushed_samples, s.resim_dropped_samples,
                               s.fill_granules, s.min_fill_granules, s.target_granules,
                               MixerFifo::ORCA_NO_FILL);
  };
  const std::string dma = part("dma", m_dma_mixer);
  const std::string dtk = part("dtk", m_streaming_mixer);

  // The four Wii-mote speakers summed; fill is the fullest one's, min the emptiest one's.
  MixerFifo::OrcaFifoStats wiimote;
  wiimote.min_fill_granules = MixerFifo::ORCA_NO_FILL;
  double wiimote_rate = 0.0;
  for (auto& fifo : m_wiimote_speaker_mixers)
  {
    const MixerFifo::OrcaFifoStats s = fifo.OrcaTakeStats();
    wiimote.underruns += s.underruns;
    wiimote.repeated_samples += s.repeated_samples;
    wiimote.skips += s.skips;
    wiimote.dropped_samples += s.dropped_samples;
    wiimote.full_drops += s.full_drops;
    wiimote.pushed_samples += s.pushed_samples;
    wiimote.resim_dropped_samples += s.resim_dropped_samples;
    if (s.fill_granules >= wiimote.fill_granules)
    {
      wiimote.fill_granules = s.fill_granules;
      wiimote.target_granules = s.target_granules;
      wiimote_rate = rate_of(fifo);
    }
    wiimote.min_fill_granules = std::min(wiimote.min_fill_granules, s.min_fill_granules);
  }
  const std::string wiimote_text = OrcaFormatFifoStats(
      "wiimote", window_s, wiimote_rate, MixerFifo::ORCA_GRANULE_HOP, wiimote.underruns,
      wiimote.repeated_samples, wiimote.skips, wiimote.dropped_samples, wiimote.full_drops,
      wiimote.pushed_samples, wiimote.resim_dropped_samples, wiimote.fill_granules,
      wiimote.min_fill_granules, wiimote.target_granules, MixerFifo::ORCA_NO_FILL);

  const u64 out_frames = m_orca_out_frames.exchange(0, std::memory_order_relaxed);
  const PerformanceMetrics& perf = Core::System::GetInstance().GetPerfMetrics();
  NOTICE_LOG_FMT(ROLLBACK,
                 "Orca mixer: t={:.0f}s window={:.1f}s fps={:.2f} vps={:.2f} speed={:.1f}% "
                 "backend_hz={} out_hz={:.0f} fill_gaps={} buffer_ms={} wav={} | {} | {} | {}",
                 since_s, window_s, perf.GetFPS(), perf.GetVPS(), perf.GetSpeed() * 100.0,
                 m_output_sample_rate, out_frames / window_s, m_config_fill_audio_gaps ? 1 : 0,
                 m_config_audio_buffer_ms, m_orca_wav_tap ? 1 : 0, dma, dtk, wiimote_text);
}
