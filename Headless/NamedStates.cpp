#include "pch.h"
#include "NamedStates.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/SaveStateManager.h"
#include "Utilities/Serializer.h"
#include "Utilities/sha1.h"
#include <cerrno>
#include <charconv>
#include <dlfcn.h>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace
{
	constexpr size_t MaxStateBytes = 64 * 1024 * 1024;
	constexpr uint16_t MaxPadMask = 0xfff;
	constexpr size_t DigestLength = 40;
	constexpr uint32_t StateVersion = SaveStateManager::FileFormatVersion;

	fs::path StatePath(const fs::path& directory, const string& name)
	{
		const auto alphaNumeric = [](char c) {
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		};
		if(name.empty() || name.size() > 64 || !alphaNumeric(name.front())
			|| !std::all_of(name.begin(), name.end(), [&](char c) {
				return alphaNumeric(c) || c == '.' || c == '_' || c == '-';
			})) {
			throw std::runtime_error("invalid named state name");
		}
		return directory / (name + ".mcs");
	}

	string Digest(const string& data)
	{
		SHA1 sha;
		sha.update(data);
		return sha.final();
	}

	string FileDigest(const char* path)
	{
		std::ifstream input(path, ios::binary);
		if(!input) {
			throw std::runtime_error("could not read the Mesen build for named state compatibility");
		}
		const string digest = SHA1::GetHash(input);
		if(input.bad()) {
			throw std::runtime_error("could not read the complete Mesen build");
		}
		return digest;
	}

	const string& BuildIdentity()
	{
		static const string identity = [] {
			Dl_info library = {};
			void* symbol = dlsym(RTLD_DEFAULT, "GetMesenVersion");
			if(!symbol || !dladdr(symbol, &library) || !library.dli_fname) {
				throw std::runtime_error("could not identify the loaded Mesen core");
			}
			return "runner " + FileDigest("/proc/self/exe") + "\ncore " + FileDigest(library.dli_fname) + "\n";
		}();
		return identity;
	}

	string CompatibilityHeader(Emulator& emu)
	{
		auto* settings = emu.GetSettings();
		auto& snes = settings->GetSnesConfig();
		Serializer settingsSerializer(StateVersion, true);
		settingsSerializer.Stream(*settings, "settings");
		settingsSerializer.Stream(snes.DisableFrameSkipping, "disableFrameSkipping");
		std::ostringstream config(ios::binary);
		settingsSerializer.SaveTo(config, 0);
		auto& rom = emu.GetRomInfo();
		return "MCE-NAMED-STATE 1\nrom " + rom.RomFile.GetSha1Hash()
			+ "\npatch " + (rom.PatchFile.IsValid() ? rom.PatchFile.GetSha1Hash() : "-")
			+ "\nconsole " + std::to_string(static_cast<unsigned>(emu.GetConsoleType()))
			+ "\nversion " + std::to_string(settings->GetVersion())
			+ "\nformat " + std::to_string(StateVersion) + "\n"
			+ BuildIdentity() + "settings " + Digest(config.str()) + "\n";
	}

	class TemporaryFile
	{
		string _path;
		int _fd = -1;

	public:
		explicit TemporaryFile(const fs::path& target) : _path(target.string() + ".XXXXXX")
		{
			_fd = mkstemp(_path.data());
			if(_fd < 0) {
				throw std::system_error(errno, std::generic_category(), "create named state temporary file");
			}
		}

		~TemporaryFile()
		{
			if(_fd >= 0) {
				close(_fd);
			}
			if(!_path.empty()) {
				unlink(_path.c_str());
			}
		}

		void Replace(const fs::path& target, const string& data)
		{
			size_t offset = 0;
			while(offset < data.size()) {
				ssize_t count = write(_fd, data.data() + offset, data.size() - offset);
				if(count < 0 && errno == EINTR) {
					continue;
				}
				if(count <= 0) {
					throw std::system_error(count < 0 ? errno : EIO, std::generic_category(), "write named state");
				}
				offset += static_cast<size_t>(count);
			}
			if(fsync(_fd) != 0) {
				throw std::system_error(errno, std::generic_category(), "sync named state");
			}
			const int result = close(_fd);
			_fd = -1;
			if(result != 0) {
				throw std::system_error(errno, std::generic_category(), "close named state");
			}
			fs::rename(_path, target);
			_path.clear();
		}
	};

	string ReadState(const fs::path& path)
	{
		if(!fs::is_regular_file(fs::symlink_status(path))) {
			throw std::runtime_error("named state is missing or unreadable");
		}
		std::ifstream input(path, ios::binary | ios::ate);
		const auto size = input.tellg();
		if(size <= 0 || size > static_cast<std::streamoff>(MaxStateBytes)) {
			throw std::runtime_error("named state is empty, unreadable, or too large");
		}
		string data(static_cast<size_t>(size), '\0');
		input.seekg(0);
		if(!input.read(data.data(), size) || input.peek() != EOF) {
			throw std::runtime_error("could not read complete named state");
		}
		return data;
	}

	template<typename T>
	T ParseNumber(std::string_view value)
	{
		T result = 0;
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
		if(parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
			throw std::runtime_error("invalid named state frame or pad");
		}
		return result;
	}

	// The core's raw serializer starts with its compression flag, then
	// repeats NUL-terminated keys and little-endian lengths plus payloads.
	bool ValidMachinePayload(std::string_view data)
	{
		if(data.size() <= 1 || data.front() != '\0') {
			return false;
		}
		data.remove_prefix(1);
		while(!data.empty()) {
			const size_t end = data.find('\0');
			if(end == std::string_view::npos || end == 0 || data.size() - end < 5) {
				return false;
			}
			data.remove_prefix(end + 1);
			uint32_t size = 0;
			for(unsigned byte = 0; byte < 4; byte++) {
				size |= static_cast<uint32_t>(static_cast<uint8_t>(data[byte])) << (byte * 8);
			}
			data.remove_prefix(4);
			if(size > data.size()) {
				return false;
			}
			data.remove_prefix(size);
		}
		return true;
	}

	string SerializeMachine(Emulator& emu)
	{
		std::ostringstream output(ios::binary);
		emu.Serialize(output, false, 0);
		return output.str();
	}

	bool RestoreMachine(Emulator& emu, const string& data)
	{
		std::istringstream input(data, ios::binary);
		return emu.Deserialize(input, StateVersion, false, emu.GetConsoleType(), false)
			== DeserializeResult::Success;
	}
}

void Headless::SaveNamedState(Emulator& emu, const fs::path& directory,
	const string& name, uint64_t frame, uint16_t pad)
{
	const auto path = StatePath(directory, name);
	const string payload = std::to_string(frame) + " " + std::to_string(pad) + "\n" + SerializeMachine(emu);
	const string data = CompatibilityHeader(emu) + Digest(payload) + "\n" + payload;
	if(data.size() > MaxStateBytes) {
		throw std::runtime_error("named state exceeds the size limit");
	}
	fs::create_directories(directory);
	TemporaryFile(path).Replace(path, data);
}

void Headless::LoadNamedState(Emulator& emu, const fs::path& directory,
	const string& name, uint64_t& frame, uint16_t& pad)
{
	const string data = ReadState(StatePath(directory, name));
	const string header = CompatibilityHeader(emu);
	const size_t payloadStart = header.size() + DigestLength + 1;
	if(data.size() <= payloadStart || data.compare(0, header.size(), header) != 0
		|| data[payloadStart - 1] != '\n') {
		throw std::runtime_error("incompatible named state (ROM, build, or settings)");
	}
	const string payload = data.substr(payloadStart);
	if(data.compare(header.size(), DigestLength, Digest(payload)) != 0) {
		throw std::runtime_error("named state checksum mismatch");
	}
	const size_t end = payload.find('\n');
	const size_t separator = payload.find(' ');
	if(end == string::npos || separator == string::npos || separator >= end) {
		throw std::runtime_error("invalid named state frame or pad");
	}
	const std::string_view values(payload);
	const auto savedFrame = ParseNumber<uint64_t>(values.substr(0, separator));
	const auto savedPad = ParseNumber<uint16_t>(values.substr(separator + 1, end - separator - 1));
	const string machine = payload.substr(end + 1);
	if(savedPad > MaxPadMask || !ValidMachinePayload(machine)) {
		throw std::runtime_error("invalid named state payload");
	}

	const string rollback = SerializeMachine(emu);
	if(!RestoreMachine(emu, machine)) {
		if(!RestoreMachine(emu, rollback)) {
			throw std::logic_error("could not recover the previous emulator state");
		}
		throw std::runtime_error("could not restore the named state");
	}
	frame = savedFrame;
	pad = savedPad;
}
