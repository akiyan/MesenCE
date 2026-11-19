#include "pch.h"
#include "Shared/Audio/SoundMixer.h"
#include "Shared/Audio/AudioPlayerHud.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Audio/SoundResampler.h"
#include "Shared/RewindManager.h"
#include "Shared/Video/VideoRenderer.h"
#include "Shared/Audio/WaveRecorder.h"
#include "Shared/Interfaces/IAudioProvider.h"
#include "Utilities/Audio/Equalizer.h"
#include "Utilities/Audio/ReverbFilter.h"
#include "Utilities/Audio/CrossFeedFilter.h"
#include <filesystem>

class AudioCapture
{
	static constexpr uint32_t MaxEvents = 1000000;

	WaveRecorder _wav;
	ofstream _events;
	bool _hasEvents;
	uint32_t _requestedFrames;
	uint32_t _lastFrame = 0;

	bool EventsGood() const
	{
		return !_hasEvents || _events.good();
	}

public:
	AudioCaptureStatus Status;

	AudioCapture(const string& wavPath, const string& eventsPath, uint32_t frames, uint32_t sampleRate)
		: _wav(wavPath, sampleRate, true), _hasEvents(!eventsPath.empty()), _requestedFrames(frames)
	{
		// An empty path records the WAV only.
		if(_hasEvents) {
			_events.open(eventsPath, ios::out | ios::binary);
		}
		Status.State = AudioCaptureState::Armed;
		Status.SampleRate = sampleRate;
	}

	bool IsOpen() const
	{
		return _wav.IsOpen() && EventsGood();
	}

	void Abort()
	{
		Status.State = AudioCaptureState::Error;
		_wav.Finish();
		if(_hasEvents) {
			_events.close();
		}
	}

	void Stop()
	{
		// Complete at the next frame end; an armed capture records one interval.
		_requestedFrames = std::min(_requestedFrames, Status.Frames + 1);
	}

	void EndFrame(uint32_t frame, uint64_t clock, uint32_t clockRate)
	{
		if(Status.State == AudioCaptureState::Armed) {
			Status.State = AudioCaptureState::Recording;
			Status.StartClock = clock;
			Status.EndClock = clock;
			Status.ClockRate = clockRate;
			_lastFrame = frame;
			if(_hasEvents) {
				_events << "{\"kind\":\"start\",\"frame\":" << frame << ",\"masterClock\":" << clock
					<< ",\"clockRate\":" << clockRate << ",\"sampleRate\":" << Status.SampleRate << "}\n";
			}
		} else if(Status.State == AudioCaptureState::Recording) {
			if(frame != _lastFrame + 1 || clock <= Status.EndClock || clockRate != Status.ClockRate) {
				Abort();
				return;
			}
			_lastFrame = frame;
			Status.EndClock = clock;
			Status.Frames++;
			if(_hasEvents) {
				_events << "{\"kind\":\"frame\",\"frame\":" << frame << ",\"masterClock\":" << clock
					<< ",\"samples\":" << Status.Samples << "}\n";
			}
			if(Status.Frames == _requestedFrames) {
				if(!_wav.Finish()) {
					Abort();
					return;
				}
				if(_hasEvents) {
					_events << "{\"kind\":\"end\",\"frames\":" << Status.Frames
						<< ",\"samples\":" << Status.Samples << ",\"events\":" << Status.Events
						<< ",\"droppedEvents\":" << Status.DroppedEvents << "}\n";
					_events.close();
				}
				Status.State = EventsGood() ? AudioCaptureState::Complete : AudioCaptureState::Error;
				return;
			}
		}
		if(!EventsGood() && Status.State == AudioCaptureState::Recording) {
			Abort();
		}
	}

	void WriteSamples(int16_t* samples, uint32_t count, uint32_t sampleRate)
	{
		if(_wav.WriteSamples(samples, count, sampleRate, true)) {
			Status.Samples += count;
		} else {
			Abort();
		}
	}

	void RecordEvent(AudioEventType type, uint64_t clock, uint64_t spcCycle, uint16_t address, uint8_t value)
	{
		if(!_hasEvents || clock < Status.StartClock) {
			return;
		}
		if(Status.Events >= MaxEvents) {
			Status.DroppedEvents++;
			return;
		}
		constexpr const char* kinds[] = { "cpu_read", "cpu_write", "spc_read", "spc_write", "dsp_write", "psg_write", "psg_stereo" };
		_events << "{\"kind\":\"" << kinds[(unsigned)type] << "\",\"masterClock\":" << clock;
		if(type < AudioEventType::PsgWrite) {
			_events << ",\"spcCycle\":" << spcCycle;
		}
		_events << ",\"address\":" << address << ",\"value\":" << (unsigned)value << "}\n";
		Status.Events++;
		if(!_events.good()) {
			Abort();
		}
	}
};

SoundMixer::SoundMixer(Emulator* emu)
{
	_emu = emu;
	_audioDevice = nullptr;
	_resampler.reset(new SoundResampler(emu));
	_sampleBuffer = new int16_t[0x10000];
	_pitchAdjustBuffer = new int16_t[0x8000];
	_reverbFilter.reset(new ReverbFilter());
	_crossFeedFilter.reset(new CrossFeedFilter());
}

SoundMixer::~SoundMixer()
{
	delete[] _sampleBuffer;
	delete[] _pitchAdjustBuffer;
}

void SoundMixer::RegisterAudioDevice(IAudioDevice* audioDevice)
{
	_audioDevice = audioDevice;
}

void SoundMixer::RegisterAudioProvider(IAudioProvider* provider)
{
	_audioProviders.push_back(provider);
}

void SoundMixer::UnregisterAudioProvider(IAudioProvider* provider)
{
	vector<IAudioProvider*>& vec = _audioProviders;
	vec.erase(std::remove(vec.begin(), vec.end(), provider), vec.end());
}

AudioStatistics SoundMixer::GetStatistics()
{
	if(_audioDevice) {
		return _audioDevice->GetStatistics();
	} else {
		return AudioStatistics();
	}
}

void SoundMixer::StopAudio(bool clearBuffer)
{
	if(_audioDevice) {
		if(clearBuffer) {
			_audioDevice->Stop();
		} else {
			_audioDevice->Pause();
		}
	}
}

void SoundMixer::PlayAudioBuffer(int16_t* samples, uint32_t sampleCount, uint32_t sourceRate)
{
	if(sampleCount == 0) {
		return;
	}

	EmuSettings* settings = _emu->GetSettings();
	AudioPlayerHud* audioPlayer = _emu->GetAudioPlayerHud();
	AudioConfig cfg = settings->GetAudioConfig();
	bool isRecording = _waveRecorder || IsAudioCaptureRecording() || _emu->GetVideoRenderer()->IsRecording();

	uint32_t masterVolume = audioPlayer ? audioPlayer->GetVolume() : cfg.MasterVolume;
	if(!isRecording) {
		if(!audioPlayer && settings->CheckFlag(EmulationFlags::InBackground)) {
			if(cfg.MuteSoundInBackground) {
				masterVolume = 0;
			} else if(cfg.ReduceSoundInBackground) {
				masterVolume = cfg.VolumeReduction == 100 ? 0 : masterVolume * (100 - cfg.VolumeReduction) / 100;
			}
		} else if(cfg.ReduceSoundInFastForward && settings->CheckFlag(EmulationFlags::TurboOrRewind)) {
			masterVolume = cfg.VolumeReduction == 100 ? 0 : masterVolume * (100 - cfg.VolumeReduction) / 100;
		}
	}

	_leftSample = samples[0];
	_rightSample = samples[1];

	int16_t* out = _sampleBuffer;
	uint32_t count = _resampler->Resample(samples, sampleCount, sourceRate, cfg.SampleRate, out, 0x10000 / 2);

	uint32_t targetRate = (uint32_t)(cfg.SampleRate * _resampler->GetRateAdjustment());
	for(IAudioProvider* provider : _audioProviders) {
		provider->MixAudio(out, count, targetRate);
	}

	if(cfg.EnableEqualizer) {
		ProcessEqualizer(out, count, targetRate);
	}

	if(audioPlayer) {
		audioPlayer->ProcessSamples(out, count, targetRate);
	}

	if(cfg.ReverbEnabled) {
		if(cfg.ReverbStrength > 0) {
			_reverbFilter->ApplyFilter(out, count, cfg.SampleRate, cfg.ReverbStrength / 10.0, cfg.ReverbDelay / 10.0);
		} else {
			_reverbFilter->ResetFilter();
		}
	}

	if(cfg.CrossFeedEnabled) {
		_crossFeedFilter->ApplyFilter(out, count, cfg.CrossFeedRatio);
	}

	if(masterVolume < 100) {
		//Apply volume if not using the default value
		for(uint32_t i = 0; i < count * 2; i++) {
			out[i] = (int32_t)out[i] * (int32_t)masterVolume / 100;
		}
	}

	RewindManager* rewindManager = _emu->GetRewindManager();
	if(!_emu->IsRunAheadFrame() && rewindManager && rewindManager->SendAudio(out, count)) {
		if(isRecording) {
			if(IsAudioCaptureRecording()) {
				_audioCapture->WriteSamples(out, count, cfg.SampleRate);
			}
			shared_ptr<WaveRecorder> recorder = _waveRecorder.lock();
			if(recorder) {
				if(!recorder->WriteSamples(out, count, cfg.SampleRate, true)) {
					StopRecording();
				}
			}
			_emu->GetVideoRenderer()->AddRecordingSound(out, count, cfg.SampleRate);
		}

		//Only send the audio to the device if the emulation is running
		//(this is to prevent playing an audio blip when loading a save state)
		if(!_emu->IsPaused() && _audioDevice) {
			if(cfg.EnableAudio) {
				uint32_t emulationSpeed = _emu->GetSettings()->GetEmulationSpeed();
				if(emulationSpeed > 0 && emulationSpeed < 100) {
					//Slow down playback when playing at less than 100% speed
					_pitchAdjust.SetSampleRates(targetRate, targetRate * 100.0 / emulationSpeed);
					count = _pitchAdjust.Resample<false>(_sampleBuffer, count, _pitchAdjustBuffer, 0x8000 / 2);
					if(count >= 0x4000) {
						//Mute sound when playing so slowly that the 64k buffer is not large enough to hold everything
						memset(_pitchAdjustBuffer, 0, 0x8000 * sizeof(int16_t));
					}
					out = _pitchAdjustBuffer;
				}

				_audioDevice->PlayBuffer(out, count, cfg.SampleRate, true);
				_audioDevice->ProcessEndOfFrame();
			} else {
				_audioDevice->Stop();
			}
		}
	}
}

void SoundMixer::ProcessEqualizer(int16_t* samples, uint32_t sampleCount, uint32_t targetRate)
{
	AudioConfig cfg = _emu->GetSettings()->GetAudioConfig();
	if(!_equalizer) {
		_equalizer.reset(new Equalizer());
	}
	vector<double> bandGains = {
		cfg.Band1Gain, cfg.Band2Gain, cfg.Band3Gain, cfg.Band4Gain, cfg.Band5Gain, cfg.Band6Gain, cfg.Band7Gain, cfg.Band8Gain, cfg.Band9Gain, cfg.Band10Gain, cfg.Band11Gain, cfg.Band12Gain, cfg.Band13Gain, cfg.Band14Gain, cfg.Band15Gain, cfg.Band16Gain, cfg.Band17Gain, cfg.Band18Gain, cfg.Band19Gain, cfg.Band20Gain
	};

	_equalizer->UpdateEqualizers(bandGains, cfg.SampleRate);
	_equalizer->ApplyEqualizer(sampleCount, samples);
}

double SoundMixer::GetRateAdjustment()
{
	return _resampler->GetRateAdjustment();
}

void SoundMixer::StartRecording(string filepath)
{
	_waveRecorder.reset(new WaveRecorder(filepath, _emu->GetSettings()->GetAudioConfig().SampleRate, true));
}

void SoundMixer::StopRecording()
{
	_waveRecorder.reset();
}

bool SoundMixer::IsRecording()
{
	return _waveRecorder != nullptr;
}

string SoundMixer::StartAudioCapture(const string& wavPath, const string& eventsPath, uint32_t frames)
{
	ConsoleType console = _emu->GetConsoleType();
	if((console != ConsoleType::Snes && console != ConsoleType::Sms) || _emu->GetSettings()->GetEmulationConfig().RunAheadFrames != 0) {
		return "audio capture requires SNES, Master System or Game Gear with run-ahead disabled";
	}
	if(frames < 1 || frames > AudioCaptureMaxFrames) {
		return "audio capture requires 1 to 1080000 frames";
	}
	if(_audioCapture && (_audioCapture->Status.State == AudioCaptureState::Armed || IsAudioCaptureRecording())) {
		return "an audio capture is already active";
	}

	namespace fs = std::filesystem;
	std::error_code wavError, eventsError;
	auto wav = fs::weakly_canonical(wavPath, wavError);
	if(wavError || wav.empty() || fs::exists(wav, wavError) || wavError) {
		return "WAV output must be a new file";
	}
	fs::path events;
	if(!eventsPath.empty()) {
		events = fs::weakly_canonical(eventsPath, eventsError);
		if(eventsError || events.empty() || wav == events || fs::exists(events, eventsError) || eventsError) {
			return "WAV and event output must be two distinct new files";
		}
	}
	auto capture = std::make_unique<AudioCapture>(wav.string(), events.string(), frames, _emu->GetSettings()->GetAudioConfig().SampleRate);
	if(!capture->IsOpen()) {
		return "could not open WAV or event output";
	}
	_audioCapture = std::move(capture);
	return {};
}

bool SoundMixer::StopAudioCapture()
{
	if(!_audioCapture || (_audioCapture->Status.State != AudioCaptureState::Armed && !IsAudioCaptureRecording())) {
		return false;
	}
	_audioCapture->Stop();
	return true;
}

AudioCaptureStatus SoundMixer::GetAudioCapture() const
{
	return _audioCapture ? _audioCapture->Status : AudioCaptureStatus {};
}

bool SoundMixer::IsAudioCaptureRecording() const
{
	return _audioCapture && _audioCapture->Status.State == AudioCaptureState::Recording;
}

void SoundMixer::EndAudioCaptureFrame(uint32_t frame, uint64_t masterClock, uint32_t clockRate)
{
	if(_audioCapture) {
		_audioCapture->EndFrame(frame, masterClock, clockRate);
	}
}

void SoundMixer::RecordAudioEvent(AudioEventType type, uint64_t masterClock, uint64_t spcCycle, uint16_t address, uint8_t value)
{
	if(IsAudioCaptureRecording()) {
		_audioCapture->RecordEvent(type, masterClock, spcCycle, address, value);
	}
}

void SoundMixer::GetLastSamples(int16_t& left, int16_t& right)
{
	left = _leftSample;
	right = _rightSample;
}
