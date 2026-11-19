#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

class Emulator;

namespace Headless
{
	// The caller owns the emulation thread at a completed frame boundary.
	void SaveNamedState(Emulator& emu, const std::filesystem::path& directory,
		const std::string& name, uint64_t frame, uint16_t pad);
	void LoadNamedState(Emulator& emu, const std::filesystem::path& directory,
		const std::string& name, uint64_t& frame, uint16_t& pad);
}
