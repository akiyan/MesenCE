#pragma once

#include <filesystem>

class Emulator;

namespace Headless
{
	void RunControlSession(Emulator& emu, const std::filesystem::path& socketPath, const std::filesystem::path& stateDirectory);
}
