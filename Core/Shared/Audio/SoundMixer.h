#pragma once
#include "pch.h"
#include "Core/Shared/Interfaces/IAudioDevice.h"
#include "Utilities/safe_ptr.h"
#include "Utilities/Audio/HermiteResampler.h"

class Emulator;
class Equalizer;
class SoundResampler;
class WaveRecorder;
class IAudioProvider;
class CrossFeedFilter;
class ReverbFilter;
class SnesAudioCapture;

enum class SnesAudioEventType { CpuRead, CpuWrite, SpcRead, SpcWrite, DspWrite };
enum class SnesAudioCaptureState { Idle, Armed, Recording, Complete, Error };

struct SnesAudioCaptureStatus
{
	SnesAudioCaptureState State = SnesAudioCaptureState::Idle;
	uint32_t SampleRate = 0;
	uint32_t ClockRate = 0;
	uint32_t Frames = 0;
	uint32_t Events = 0;
	uint64_t DroppedEvents = 0;
	uint64_t StartClock = 0;
	uint64_t EndClock = 0;
	uint64_t Samples = 0;
};

class SoundMixer
{
private:
	IAudioDevice* _audioDevice;
	vector<IAudioProvider*> _audioProviders;
	Emulator* _emu;
	unique_ptr<Equalizer> _equalizer;
	unique_ptr<SoundResampler> _resampler;
	safe_ptr<WaveRecorder> _waveRecorder;
	int16_t* _sampleBuffer = nullptr;

	HermiteResampler _pitchAdjust;
	int16_t* _pitchAdjustBuffer = nullptr;

	int16_t _leftSample = 0;
	int16_t _rightSample = 0;

	unique_ptr<CrossFeedFilter> _crossFeedFilter;
	unique_ptr<ReverbFilter> _reverbFilter;
	unique_ptr<SnesAudioCapture> _snesAudioCapture;

	void ProcessEqualizer(int16_t* samples, uint32_t sampleCount, uint32_t targetRate);

public:
	SoundMixer(Emulator* emu);
	~SoundMixer();

	void PlayAudioBuffer(int16_t* samples, uint32_t sampleCount, uint32_t sourceRate);
	void StopAudio(bool clearBuffer = false);

	void RegisterAudioDevice(IAudioDevice* audioDevice);

	void RegisterAudioProvider(IAudioProvider* provider);
	void UnregisterAudioProvider(IAudioProvider* provider);

	AudioStatistics GetStatistics();
	double GetRateAdjustment();

	void StartRecording(string filepath);
	void StopRecording();
	bool IsRecording();
	string StartSnesAudioCapture(const string& wavPath, const string& eventsPath, uint32_t frames);
	SnesAudioCaptureStatus GetSnesAudioCapture() const;
	bool IsSnesAudioCaptureRecording() const;
	void EndSnesAudioFrame(uint32_t frame, uint64_t masterClock, uint32_t clockRate);
	void RecordSnesAudioEvent(SnesAudioEventType type, uint64_t masterClock, uint64_t spcCycle, uint16_t address, uint8_t value);
	void GetLastSamples(int16_t& left, int16_t& right);
};
