#include "pch.h"
#include "Debugger/Debugger.h"
#include "Debugger/LuaApi.h"
#include "Debugger/MemoryDumper.h"
#include "Lua/lua.hpp"
#include "Shared/ColorUtilities.h"
#include "Shared/Emulator.h"
#include "Shared/MemoryOperationType.h"
#include "SNES/Debugger/SnesEventManager.h"
#include "SNES/SnesConsole.h"
#include "SNES/SnesCpu.h"
#include "SNES/SnesPpu.h"
#include "Utilities/PNGHelper.h"

namespace
{
	SnesEventManager* GetRasterObserver(lua_State* lua, Debugger* debugger)
	{
		if(debugger->GetEmulator()->GetConsoleType() == ConsoleType::Snes) {
			if(auto* manager = debugger->GetEventManager(CpuType::Snes)) {
				return static_cast<SnesEventManager*>(manager);
			}
		}
		luaL_error(lua, "SNES observation requires a loaded SNES ROM");
		return nullptr;
	}

	void SetInteger(lua_State* lua, const char* key, lua_Integer value)
	{
		lua_pushinteger(lua, value);
		lua_setfield(lua, -2, key);
	}

	void SetBoolean(lua_State* lua, const char* key, bool value)
	{
		lua_pushboolean(lua, value);
		lua_setfield(lua, -2, key);
	}

	void SetString(lua_State* lua, const char* key, const char* value)
	{
		lua_pushstring(lua, value);
		lua_setfield(lua, -2, key);
	}

	void SetTiming(lua_State* lua, const SnesRasterTiming& timing)
	{
		SetInteger(lua, "masterClock", timing.MasterClock);
		SetInteger(lua, "frame", timing.Frame);
		SetInteger(lua, "scanline", timing.Scanline);
		SetInteger(lua, "cycle", timing.Cycle);
	}

	void PushCpu(lua_State* lua, const SnesCpuState& state)
	{
		lua_createtable(lua, 0, 10);
		SetInteger(lua, "pc", (uint32_t(state.K) << 16) | state.PC);
		SetInteger(lua, "a", state.A);
		SetInteger(lua, "x", state.X);
		SetInteger(lua, "y", state.Y);
		SetInteger(lua, "sp", state.SP);
		SetInteger(lua, "directPage", state.D);
		SetInteger(lua, "dataBank", state.DBR);
		SetInteger(lua, "status", state.PS);
		SetInteger(lua, "cycleCount", state.CycleCount);
		SetBoolean(lua, "emulationMode", state.EmulationMode);
	}

	void PushPpu(lua_State* lua, const SnesPpuState& state, const uint8_t* cgram)
	{
		lua_createtable(lua, 0, 11);
		SetInteger(lua, "dot", state.Cycle);
		SetInteger(lua, "bgMode", state.BgMode);
		SetInteger(lua, "brightness", state.ScreenBrightness);
		SetInteger(lua, "mainScreenLayers", state.MainScreenLayers);
		SetInteger(lua, "subScreenLayers", state.SubScreenLayers);
		SetBoolean(lua, "forcedBlank", state.ForcedBlank);
		SetBoolean(lua, "overscan", state.OverscanMode);
		SetBoolean(lua, "interlace", state.ScreenInterlace);
		SetBoolean(lua, "hiRes", state.HiResMode);

		lua_createtable(lua, 4, 0);
		for(int i = 0; i < 4; i++) {
			const LayerConfig& layer = state.Layers[i];
			lua_createtable(lua, 0, 7);
			SetInteger(lua, "tilemapWordAddress", layer.TilemapAddress);
			SetInteger(lua, "tileDataWordAddress", layer.ChrAddress);
			SetInteger(lua, "horizontalScroll", layer.HScroll);
			SetInteger(lua, "verticalScroll", layer.VScroll);
			SetBoolean(lua, "doubleWidth", layer.DoubleWidth);
			SetBoolean(lua, "doubleHeight", layer.DoubleHeight);
			SetBoolean(lua, "largeTiles", layer.LargeTiles);
			lua_rawseti(lua, -2, i + 1);
		}
		lua_setfield(lua, -2, "layers");

		lua_createtable(lua, SnesPpu::CgRamSize / 2, 0);
		for(uint32_t i = 0; i < SnesPpu::CgRamSize / 2; i++) {
			lua_pushinteger(lua, cgram[i * 2] | ((cgram[i * 2 + 1] & 0x7F) << 8));
			lua_rawseti(lua, -2, i + 1);
		}
		lua_setfield(lua, -2, "palette");
	}

	void PushCompletedFrame(lua_State* lua, const SnesCompletedFrame& frame)
	{
		lua_createtable(lua, 0, 7);
		SetTiming(lua, frame.Timing);
		SetInteger(lua, "width", frame.Width);
		SetInteger(lua, "height", frame.Height);

		vector<uint32_t> pixels;
		pixels.reserve(frame.Pixels.size());
		for(uint16_t pixel : frame.Pixels) {
			pixels.push_back(ColorUtilities::Rgb555ToArgb(pixel));
		}
		stringstream stream;
		if(PNGHelper::WritePNG(stream, pixels.data(), frame.Width, frame.Height)) {
			string png = stream.str();
			lua_pushlstring(lua, png.data(), png.size());
			lua_setfield(lua, -2, "png");
		}
	}

	void PushTraceEvent(lua_State* lua, const SnesRasterEvent& event)
	{
		lua_createtable(lua, 0, 9);
		SetTiming(lua, event.Timing);
		if(event.Type == DebugEventType::Nmi || event.Type == DebugEventType::Irq) {
			SetString(lua, "kind", event.Type == DebugEventType::Nmi ? "nmi" : "irq");
			return;
		}

		SetString(lua, "kind", event.DmaChannel < 0 ? "ppu" : (event.IsHdma ? "hdma" : "dma"));
		MemoryOperationType type = event.Operation.Type;
		bool write = type == MemoryOperationType::Write || type == MemoryOperationType::DmaWrite || type == MemoryOperationType::DummyWrite;
		SetString(lua, "access", write ? "write" : "read");
		SetInteger(lua, "address", event.Operation.Address);
		SetInteger(lua, "value", event.Operation.Value);
		if(event.DmaChannel >= 0) {
			SetInteger(lua, "channel", event.DmaChannel);
		}
	}
}

int LuaApi::EnableSnesFrameCapture(lua_State* lua)
{
	if(lua_gettop(lua) != 1 || !lua_isboolean(lua, 1)) {
		return luaL_error(lua, "expected one boolean to enable or disable SNES frame capture");
	}
	GetRasterObserver(lua, _debugger)->EnableFrameObservation(lua_toboolean(lua, 1));
	return 0;
}

int LuaApi::GetSnesPpuSnapshot(lua_State* lua)
{
	SnesEventManager* observer = GetRasterObserver(lua, _debugger);
	lua_Integer wramOffset = luaL_optinteger(lua, 1, 0);
	lua_Integer wramLength = luaL_optinteger(lua, 2, 0);
	uint32_t wramSize = _memoryDumper->GetMemorySize(MemoryType::SnesWorkRam);
	if(lua_gettop(lua) > 2 || wramOffset < 0 || wramLength < 0 || wramLength > 4096 || wramOffset > wramSize - wramLength) {
		return luaL_error(lua, "expected a valid WRAM offset and a length of up to 4096 bytes");
	}

	auto* console = static_cast<SnesConsole*>(_debugger->GetConsole());
	SnesPpu* ppu = console->GetPpu();
	const SnesRasterTiming timing = observer->GetRasterTiming();
	const char* boundary = "emulation";
	if(_debugger->IsExecutionStopped()) {
		boundary = "debugger";
	} else if(timing.Cycle == 0 && timing.Scanline == 0) {
		boundary = "frameStart";
	} else if(timing.Cycle == 0 && timing.Scanline == ppu->GetNmiScanline()) {
		boundary = "vblankStart";
	}

	lua_createtable(lua, 0, 5);
	lua_createtable(lua, 0, 5);
	SetTiming(lua, timing);
	SetString(lua, "boundary", boundary);
	lua_setfield(lua, -2, "live");

	PushCpu(lua, console->GetCpu()->GetState());
	lua_setfield(lua, -2, "cpu");
	PushPpu(lua, ppu->GetState(), ppu->GetCgRam());
	lua_setfield(lua, -2, "ppu");

	lua_createtable(lua, 0, 2);
	SetInteger(lua, "offset", wramOffset);
	lua_createtable(lua, (int)wramLength, 0);
	for(lua_Integer i = 0; i < wramLength; i++) {
		lua_pushinteger(lua, _memoryDumper->GetMemoryValue(MemoryType::SnesWorkRam, uint32_t(wramOffset + i)));
		lua_rawseti(lua, -2, i + 1);
	}
	lua_setfield(lua, -2, "bytes");
	lua_setfield(lua, -2, "wram");

	const SnesCompletedFrame& frame = observer->GetCompletedFrame();
	if(!frame.Pixels.empty()) {
		PushCompletedFrame(lua, frame);
		lua_setfield(lua, -2, "completedFrame");
	}
	return 1;
}

int LuaApi::StartSnesPpuTrace(lua_State* lua)
{
	lua_Integer capacity = luaL_checkinteger(lua, 1);
	if(lua_gettop(lua) != 1 || capacity < 1 || capacity > SnesEventManager::MaxRasterTraceEvents) {
		return luaL_error(lua, "expected a SNES raster trace capacity of 1-65536 events");
	}
	GetRasterObserver(lua, _debugger)->StartRasterTrace(uint32_t(capacity));
	return 0;
}

int LuaApi::StopSnesPpuTrace(lua_State* lua)
{
	if(lua_gettop(lua) != 0) {
		return luaL_error(lua, "expected no arguments");
	}
	SnesRasterTrace trace = GetRasterObserver(lua, _debugger)->StopRasterTrace();
	lua_createtable(lua, 0, 2);
	SetInteger(lua, "dropped", trace.Dropped);
	lua_createtable(lua, (int)trace.Events.size(), 0);
	for(size_t i = 0; i < trace.Events.size(); i++) {
		PushTraceEvent(lua, trace.Events[i]);
		lua_rawseti(lua, -2, i + 1);
	}
	lua_setfield(lua, -2, "events");
	return 1;
}
