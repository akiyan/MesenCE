#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/BaseEventManager.h"
#include "Utilities/SimpleLock.h"

enum class DebugEventType;
struct DebugEventInfo;
struct EventViewerDisplayOptions;
class SnesCpu;
class SnesPpu;
class Debugger;
class SnesDmaController;
class SnesMemoryManager;

struct SnesRasterTiming
{
	uint64_t MasterClock = 0;
	uint32_t Frame = 0;
	uint16_t Scanline = 0;
	uint16_t Cycle = 0;
};

struct SnesRasterEvent
{
	SnesRasterTiming Timing;
	DebugEventType Type;
	MemoryOperationInfo Operation;
	int8_t DmaChannel = -1;
	bool IsHdma = false;
};

struct SnesRasterTrace
{
	vector<SnesRasterEvent> Events;
	uint64_t Dropped = 0;
};

struct SnesCompletedFrame
{
	SnesRasterTiming Timing;
	uint16_t Width = 0;
	uint16_t Height = 0;
	vector<uint16_t> Pixels;
};

struct SnesEventViewerConfig : public BaseEventViewerConfig
{
	EventViewerCategoryCfg Irq;
	EventViewerCategoryCfg Nmi;
	EventViewerCategoryCfg MarkedBreakpoints;

	EventViewerCategoryCfg PpuRegisterReads;
	EventViewerCategoryCfg PpuRegisterCgramWrites;
	EventViewerCategoryCfg PpuRegisterVramWrites;
	EventViewerCategoryCfg PpuRegisterOamWrites;
	EventViewerCategoryCfg PpuRegisterMode7Writes;
	EventViewerCategoryCfg PpuRegisterBgOptionWrites;
	EventViewerCategoryCfg PpuRegisterBgScrollWrites;
	EventViewerCategoryCfg PpuRegisterWindowWrites;
	EventViewerCategoryCfg PpuRegisterOtherWrites;

	EventViewerCategoryCfg ApuRegisterReads;
	EventViewerCategoryCfg ApuRegisterWrites;
	EventViewerCategoryCfg CpuRegisterReads;
	EventViewerCategoryCfg CpuRegisterWrites;
	EventViewerCategoryCfg WorkRamRegisterReads;
	EventViewerCategoryCfg WorkRamRegisterWrites;

	bool ShowPreviousFrameEvents;
	uint8_t ShowDmaChannels[8];
};

class SnesEventManager final : public BaseEventManager
{
private:
	static constexpr int ScanlineWidth = 1364 / 2;

	SnesEventViewerConfig _config;

	SnesCpu* _cpu;
	SnesPpu* _ppu;
	SnesMemoryManager* _memoryManager;
	SnesDmaController* _dmaController;
	Debugger* _debugger;

	bool _overscanMode = false;
	bool _useHighResOutput = false;
	uint32_t _scanlineCount = 262;
	uint16_t* _ppuBuffer = nullptr;

	bool _captureCompletedFrames = false;
	SnesCompletedFrame _completedFrame;
	uint32_t _rasterTraceCapacity = 0;
	SnesRasterTrace _rasterTrace;

	void RecordRasterEvent(SnesRasterEvent event);

protected:
	void ConvertScanlineCycleToRowColumn(int32_t& x, int32_t& y) override;
	void DrawScreen(uint32_t* buffer) override;
	bool ShowPreviousFrameEvents() override;

public:
	SnesEventManager(Debugger* debugger, SnesCpu* cpu, SnesPpu* ppu, SnesMemoryManager* memoryManager, SnesDmaController* dmaController);
	~SnesEventManager();

	void AddEvent(DebugEventType type, MemoryOperationInfo& operation, int32_t breakpointId = -1) override;
	void AddEvent(DebugEventType type) override;

	// Call these from an emulation callback or while the emulator is held.
	static constexpr uint32_t MaxRasterTraceEvents = 65536;
	void StartRasterTrace(uint32_t capacity);
	SnesRasterTrace StopRasterTrace();
	bool IsRasterTraceActive() const { return _rasterTraceCapacity != 0; }
	void RecordRasterBusOperation(const MemoryOperationInfo& operation, bool isRegister);
	SnesRasterTiming GetRasterTiming();
	void EnableFrameObservation(bool enabled);
	void RecordCompletedFrame(const uint16_t* pixels, uint16_t width, uint16_t height);
	const SnesCompletedFrame& GetCompletedFrame() const { return _completedFrame; }
	void ResetRasterObservation();

	EventViewerCategoryCfg GetEventConfig(DebugEventInfo& evt) override;

	uint32_t TakeEventSnapshot(bool forAutoRefresh) override;
	FrameInfo GetDisplayBufferSize() override;
	DebugEventInfo GetEvent(uint16_t y, uint16_t x) override;

	void SetConfiguration(BaseEventViewerConfig& config) override;
};
