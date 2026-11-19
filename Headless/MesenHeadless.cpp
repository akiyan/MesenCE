#include "pch.h"
#include <charconv>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include "ControlSession.h"
#include "Debugger/Debugger.h"
#include "Debugger/ScriptManager.h"
#include "Shared/DebuggerRequest.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Shared/NotificationManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/VirtualFile.h"

namespace fs = std::filesystem;

namespace
{
	class StopObserver final : public INotificationListener
	{
		std::mutex _mutex;
		std::condition_variable _condition;
		bool _stopped = false;

	public:
		void ProcessNotification(ConsoleNotificationType type, void*) override
		{
			if(type == ConsoleNotificationType::EmulationStopped) {
				std::lock_guard lock(_mutex);
				_stopped = true;
				_condition.notify_one();
			}
		}

		void Wait()
		{
			std::unique_lock lock(_mutex);
			_condition.wait(lock, [this] { return _stopped; });
		}
	};

	struct EmulatorSession
	{
		Emulator Core;

		EmulatorSession() { Core.Initialize(false); }
		~EmulatorSession() { Core.Release(); }
	};

	void Timeout(int)
	{
		// The deadline also covers Lua waiting in a blocking I/O call.
		constexpr char message[] = "mesen-headless: timed out\n";
		[[maybe_unused]] ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
		_exit(124);
	}

	int Usage()
	{
		std::cerr << "usage: mesen-headless --home ABS_DIR [--timeout SECONDS] ROM LUA\n"
			<< "       mesen-headless --home ABS_DIR [--state-dir ABS_DIR] --control ABS_SOCKET ROM\n";
		return 2;
	}

	void PrepareProfile(const fs::path& home)
	{
		fs::create_directories(home);
		// Reserve the empty profile against a second invocation using the same path.
		if(!fs::is_empty(home) || !fs::create_directory(home / "Saves")) {
			throw std::runtime_error("profile directory must be empty: " + home.string());
		}

		fs::current_path(home);
		FolderUtilities::SetHomeFolder(home.string());
	}

	void ConfigureEmulator(Emulator& emu)
	{
		auto* settings = emu.GetSettings();
		settings->SetFlag(EmulationFlags::ConsoleMode);
		// Run unthrottled. Loading a ROM resets the rewind manager, which clears the
		// MaximumSpeed flag, so the speed setting itself is 0 (no frame delay).
		settings->SetFlag(EmulationFlags::MaximumSpeed);
		settings->GetEmulationConfig().EmulationSpeed = 0;
		auto& snes = settings->GetSnesConfig();
		snes.Port1.Type = ControllerType::SnesController;
		snes.DisableFrameSkipping = true;
		snes.RamPowerOnState = RamState::AllZeros;

		// Master System and Game Gear: the standard pad, zeroed RAM, the visible
		// screen of each model and full PSG channel volumes (the UI defaults; the
		// core's are 0). A Game Gear frame is not blended with the previous one,
		// so every screenshot shows a single frame.
		auto& sms = settings->GetSmsConfig();
		sms.Port1.Type = ControllerType::SmsController;
		for(uint32_t& volume : sms.ChannelVolumes) {
			volume = 100;
		}
		sms.RamPowerOnState = RamState::AllZeros;
		sms.GgBlendFrames = false;
		sms.NtscOverscan = { 0, 0, 24, 24 };
		sms.PalOverscan = { 0, 0, 24, 24 };
		sms.GameGearOverscan = { 48, 48, 48, 48 };
	}

	int Run(const fs::path& home, const fs::path& romPath, const fs::path& scriptPath, unsigned timeout)
	{
		std::signal(SIGALRM, Timeout);
		alarm(timeout);

		std::ifstream scriptFile(scriptPath, std::ios::binary);
		if(!scriptFile) {
			throw std::runtime_error("could not open Lua script " + scriptPath.string());
		}
		string scriptContent((std::istreambuf_iterator<char>(scriptFile)), {});

		PrepareProfile(home);
		EmulatorSession session;
		Emulator& emu = session.Core;
		ConfigureEmulator(emu);
		auto* settings = emu.GetSettings();
		auto& debug = settings->GetDebugConfig();
		debug.ScriptAllowIoOsAccess = true;
		debug.ScriptTimeout = timeout;

		auto stopObserver = std::make_shared<StopObserver>();
		emu.GetNotificationManager()->RegisterNotificationListener(stopObserver);

		emu.Pause();
		if(!emu.LoadRom(VirtualFile(romPath.string()), VirtualFile())) {
			throw std::runtime_error("could not load ROM " + romPath.string());
		}

		{
			auto request = emu.GetDebugger();
			auto* scripts = request.GetDebugger()->GetScriptManager();
			bool loaded;
			int32_t id = scripts->LoadScript(scriptPath.string(), scriptPath.parent_path().string() + "/", std::move(scriptContent), -1, loaded);
			if(!loaded) {
				throw std::runtime_error("could not load Lua script\n" + scripts->GetScriptLog(id));
			}
		}

		emu.Resume();
		stopObserver->Wait();
		return emu.GetStopCode();
	}

	int RunControl(const fs::path& home, const fs::path& romPath, const fs::path& socketPath, const fs::path& stateDirectory)
	{
		// Restrict the local socket and the session's files to its owner.
		umask(S_IRWXG | S_IRWXO);
		PrepareProfile(home);
		EmulatorSession session;
		Emulator& emu = session.Core;
		ConfigureEmulator(emu);
		// The frame-control loop runs the emulation thread directly.
		if(!emu.LoadRom(VirtualFile(romPath.string()), VirtualFile(), false)) {
			throw std::runtime_error("could not load ROM " + romPath.string());
		}
		if(emu.GetConsoleType() != ConsoleType::Snes) {
			throw std::runtime_error("control mode requires a SNES ROM");
		}
		Headless::RunControlSession(emu, socketPath, stateDirectory);
		return emu.GetStopCode();
	}
}

int main(int argc, char** argv)
{
	if(argc < 5 || std::string_view(argv[1]) != "--home") {
		return Usage();
	}

	fs::path home(argv[2]);
	if(!home.is_absolute()) {
		return Usage();
	}

	const int controlArg = argc == 6 ? 3 : (argc == 8 && std::string_view(argv[3]) == "--state-dir" ? 5 : 0);
	if(controlArg && std::string_view(argv[controlArg]) == "--control") {
		fs::path socketPath(argv[controlArg + 1]);
		fs::path stateDirectory = controlArg == 5 ? fs::path(argv[4]) : home / "NamedStates";
		if(!socketPath.is_absolute() || !stateDirectory.is_absolute()) {
			return Usage();
		}
		try {
			auto romPath = fs::absolute(argv[controlArg + 2]);
			if(!fs::is_regular_file(romPath)) {
				throw std::runtime_error("ROM must be a regular file");
			}
			return RunControl(home, romPath, socketPath, stateDirectory);
		} catch(const std::exception& ex) {
			std::cerr << "mesen-headless: " << ex.what() << '\n';
			return 1;
		}
	}

	if(argc != 5 && argc != 7) {
		return Usage();
	}
	unsigned timeout = 30;
	int romArg = 3;
	if(argc == 7) {
		std::string_view argument(argv[4]);
		const char* end = argument.data() + argument.size();
		auto parsed = std::from_chars(argument.data(), end, timeout);
		if(std::string_view(argv[3]) != "--timeout" || parsed.ec != std::errc{} || parsed.ptr != end || timeout == 0) {
			return Usage();
		}
		romArg = 5;
	}

	try {
		auto romPath = fs::absolute(argv[romArg]);
		auto scriptPath = fs::absolute(argv[romArg + 1]);
		if(!fs::is_regular_file(romPath) || !fs::is_regular_file(scriptPath)) {
			throw std::runtime_error("ROM and Lua script must be regular files");
		}
		int result = Run(home, romPath, scriptPath, timeout);
		alarm(0);
		return result;
	} catch(const std::exception& ex) {
		alarm(0);
		std::cerr << "mesen-headless: " << ex.what() << '\n';
		return 1;
	}
}
