#include "pch.h"
#include <cerrno>
#include <charconv>
#include <limits>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "ControlSession.h"
#include "NamedStates.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/Emulator.h"
#include "Shared/Interfaces/IInputProvider.h"
#include "Shared/MemoryType.h"

namespace fs = std::filesystem;

namespace
{
	using Clock = std::chrono::steady_clock;
	constexpr char HexDigits[] = "0123456789abcdef";
	constexpr uint16_t PadMask = 0x0fff;
	constexpr size_t MaxRequestSize = 4096;
	constexpr size_t MaxOutputSize = 65536;

	class FileDescriptor
	{
		int _fd = -1;

	public:
		FileDescriptor() = default;
		FileDescriptor(const FileDescriptor&) = delete;
		FileDescriptor& operator=(const FileDescriptor&) = delete;
		~FileDescriptor() { Reset(); }

		int Get() const { return _fd; }
		explicit operator bool() const { return _fd >= 0; }

		void Reset(int fd = -1)
		{
			if(_fd >= 0) {
				close(_fd);
			}
			_fd = fd;
		}
	};

	std::runtime_error SocketError(const char* action)
	{
		return std::runtime_error(string(action) + ": " + std::strerror(errno));
	}

	template<typename T>
	T ParseNumber(std::string_view input, const char* name)
	{
		T value{};
		int base = 10;
		if(input.substr(0, 2) == "0x") {
			input.remove_prefix(2);
			base = 16;
		}
		const char* end = input.data() + input.size();
		auto parsed = std::from_chars(input.data(), end, value, base);
		if(parsed.ec != std::errc{} || parsed.ptr != end) {
			throw std::runtime_error(string("invalid ") + name);
		}
		return value;
	}

	string Quote(std::string_view value)
	{
		string result = "\"";
		for(unsigned char ch : value) {
			if(ch == '"' || ch == '\\') {
				result += '\\';
				result += ch;
			} else if(ch < 0x20) {
				result += "\\u00";
				result += HexDigits[ch >> 4];
				result += HexDigits[ch & 0x0f];
			} else {
				result += ch;
			}
		}
		return result + '"';
	}

	struct FrameRun
	{
		uint64_t Id;
		uint64_t Requested;
		uint64_t Advanced;
		Clock::time_point Deadline;
	};

	class ControlSession final : public IInputProvider
	{
		Emulator& _emu;
		const fs::path _socketPath;
		const fs::path _stateDirectory;
		FileDescriptor _listener;
		FileDescriptor _client;
		bool _bound = false;
		bool _quit = false;
		uint64_t _frame = 0;
		uint16_t _pad = 0;
		optional<FrameRun> _run;
		string _lastStop = "initial";
		string _input;
		string _output;

	public:
		ControlSession(Emulator& emu, const fs::path& socketPath, const fs::path& stateDirectory)
			: _emu(emu), _socketPath(socketPath), _stateDirectory(stateDirectory)
		{
		}

		~ControlSession()
		{
			if(_bound) {
				std::error_code error;
				if(fs::is_socket(fs::symlink_status(_socketPath, error))) {
					fs::remove(_socketPath, error);
				}
			}
		}

		void Listen()
		{
			sockaddr_un address{};
			address.sun_family = AF_UNIX;
			string path = _socketPath.string();
			const auto relative = _socketPath.lexically_relative(fs::current_path());
			if(!relative.empty() && relative.native().size() < path.size()) {
				path = relative.string();
			}
			if(path.size() >= sizeof(address.sun_path)) {
				throw std::runtime_error("control socket path is too long");
			}
			std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

			_listener.Reset(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
			if(!_listener) {
				throw SocketError("could not create control socket");
			}
			if(bind(_listener.Get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
				throw SocketError("could not bind control socket");
			}
			_bound = true;
			if(listen(_listener.Get(), 1) < 0) {
				throw SocketError("could not listen on control socket");
			}
		}

		bool SetInput(BaseControlDevice* device) override
		{
			if(device->GetPort() != 0 || device->GetControllerType() != ControllerType::SnesController) {
				return false;
			}
			for(uint8_t bit = 0; bit < 12; bit++) {
				device->SetBitValue(bit, (_pad & (1 << bit)) != 0);
			}
			return true;
		}

		bool BeforeFrame()
		{
			if(_run) {
				++_frame;
				if(++_run->Advanced == _run->Requested) {
					Finish("completed");
				}
			}
			while(!_quit) {
				CheckDeadline();
				Poll(_run ? 0 : -1);
				CheckDeadline();
				if(_run) {
					return true;
				}
			}
			return false;
		}

	private:
		string Position() const
		{
			return ",\"frame\":" + std::to_string(_frame)
				+ ",\"boundary\":\"frame\",\"masterClock\":" + std::to_string(_emu.GetMasterClock())
				+ ",\"pad\":" + std::to_string(_pad);
		}

		void Disconnect()
		{
			if(_run) {
				_lastStop = "disconnected";
				_run.reset();
			}
			_client.Reset();
			_input.clear();
			_output.clear();
		}

		void Flush()
		{
			while(_client && !_output.empty()) {
				ssize_t size = send(_client.Get(), _output.data(), _output.size(), MSG_NOSIGNAL);
				if(size > 0) {
					_output.erase(0, size);
				} else if(size < 0 && errno == EINTR) {
					continue;
				} else {
					if(size == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
						Disconnect();
					}
					break;
				}
			}
		}

		void Reply(optional<uint64_t> id, bool ok, const string& fields = "")
		{
			if(!_client) {
				return;
			}
			_output += "{\"id\":" + (id ? std::to_string(*id) : "null")
				+ ",\"ok\":" + (ok ? "true" : "false") + fields + "}\n";
			if(_output.size() > MaxOutputSize) {
				Disconnect();
			} else {
				Flush();
			}
		}

		void Finish(const char* reason)
		{
			const FrameRun run = *_run;
			_run.reset();
			_lastStop = reason;
			Reply(run.Id, true, Position() + ",\"advanced\":" + std::to_string(run.Advanced) + ",\"reason\":" + Quote(reason));
		}

		void CheckDeadline()
		{
			if(_run && Clock::now() >= _run->Deadline) {
				Finish("deadline");
			}
		}

		void Handle(const string& line)
		{
			std::istringstream input(line);
			string requestId;
			string command;
			input >> requestId >> command;
			optional<uint64_t> id;
			try {
				id = ParseNumber<uint64_t>(requestId, "request id");
				vector<string> args;
				for(string arg; input >> arg;) {
					args.push_back(std::move(arg));
				}
				Execute(*id, command, args);
			} catch(const std::exception& ex) {
				Reply(id, false, ",\"error\":" + Quote(ex.what()));
			}
		}

		void Execute(uint64_t id, const string& command, const vector<string>& args)
		{
			if(command == "status" && args.empty()) {
				Reply(id, true, Position() + ",\"lastStop\":" + Quote(_lastStop)
					+ ",\"running\":" + (_run ? "true" : "false"));
				return;
			}
			if(command == "quit" && args.empty()) {
				if(_run) {
					Finish("cancelled");
				}
				Reply(id, true);
				_quit = true;
				return;
			}
			if(command == "cancel" && args.empty()) {
				if(!_run) {
					throw std::runtime_error("no frame run is active");
				}
				const uint64_t advanced = _run->Advanced;
				Finish("cancelled");
				Reply(id, true, Position() + ",\"advanced\":" + std::to_string(advanced) + ",\"reason\":\"cancelled\"");
				return;
			}
			if(_run) {
				throw std::runtime_error("the command requires a held session");
			}

			if(command == "run" && (args.size() == 1 || args.size() == 2)) {
				const uint64_t frames = ParseNumber<uint64_t>(args[0], "frame count");
				const unsigned timeout = args.size() == 2 ? ParseNumber<unsigned>(args[1], "deadline in milliseconds") : 30000;
				if(frames == 0 || frames > std::numeric_limits<uint64_t>::max() - _frame || timeout == 0) {
					throw std::runtime_error("frame count and deadline must be in range");
				}
				_run = FrameRun{ id, frames, 0, Clock::now() + std::chrono::milliseconds(timeout) };
			} else if(command == "pad" && args.size() == 1) {
				const uint16_t pad = ParseNumber<uint16_t>(args[0], "pad mask");
				if(pad > PadMask) {
					throw std::runtime_error("pad mask exceeds 12 buttons");
				}
				_pad = pad;
				Reply(id, true, Position());
			} else if(command == "read" && args.size() == 2) {
				ReadMemory(id, args);
			} else if(command == "save" && args.size() == 1) {
				Headless::SaveNamedState(_emu, _stateDirectory, args[0], _frame, _pad);
				Reply(id, true, Position());
			} else if(command == "load" && args.size() == 1) {
				Headless::LoadNamedState(_emu, _stateDirectory, args[0], _frame, _pad);
				_lastStop = "loaded";
				Reply(id, true, Position());
			} else {
				throw std::runtime_error("unknown command or wrong argument count");
			}
		}

		void ReadMemory(uint64_t id, const vector<string>& args)
		{
			const uint32_t address = ParseNumber<uint32_t>(args[0], "WRAM offset");
			const uint32_t count = ParseNumber<uint32_t>(args[1], "byte count");
			const auto wram = _emu.GetMemory(MemoryType::SnesWorkRam);
			if(count == 0 || count > 4096 || address >= wram.Size || count > wram.Size - address) {
				throw std::runtime_error("WRAM read range is invalid");
			}
			const auto* bytes = static_cast<const uint8_t*>(wram.Memory) + address;
			string data;
			data.reserve(count * 2);
			for(uint32_t i = 0; i < count; i++) {
				data += HexDigits[bytes[i] >> 4];
				data += HexDigits[bytes[i] & 0x0f];
			}
			Reply(id, true, Position() + ",\"data\":" + Quote(data));
		}

		void Receive()
		{
			char buffer[MaxRequestSize];
			ssize_t length = recv(_client.Get(), buffer, sizeof(buffer), 0);
			if(length <= 0) {
				if(length == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
					Disconnect();
				}
				return;
			}
			_input.append(buffer, length);
			while(_client && !_quit) {
				size_t end = _input.find('\n');
				if(end == string::npos) {
					break;
				}
				if(end > MaxRequestSize) {
					Disconnect();
					return;
				}
				string line = _input.substr(0, end);
				_input.erase(0, end + 1);
				Handle(line);
			}
			if(_input.size() > MaxRequestSize) {
				Disconnect();
			}
		}

		void Poll(int timeout)
		{
			pollfd fd{ _client ? _client.Get() : _listener.Get(), POLLIN, 0 };
			if(!_output.empty()) {
				fd.events |= POLLOUT;
			}
			int result = poll(&fd, 1, timeout);
			if(result < 0 && errno != EINTR) {
				throw SocketError("could not poll control socket");
			}
			if(result <= 0) {
				return;
			}
			if(!_client) {
				int accepted = accept4(_listener.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
				if(accepted >= 0) {
					_client.Reset(accepted);
				} else if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
					throw SocketError("could not accept control client");
				}
				return;
			}
			if(fd.revents & POLLIN) {
				Receive();
			}
			if(fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				Disconnect();
			} else if(fd.revents & POLLOUT) {
				Flush();
			}
		}
	};
}

void Headless::RunControlSession(Emulator& emu, const fs::path& socketPath, const fs::path& stateDirectory)
{
	ControlSession session(emu, socketPath, stateDirectory);
	session.Listen();
	emu.RegisterInputProvider(&session);
	std::exception_ptr failure;
	emu.RunWithFrameBoundary([&]() {
		try {
			return session.BeforeFrame();
		} catch(...) {
			failure = std::current_exception();
			return false;
		}
	});
	emu.UnregisterInputProvider(&session);
	if(failure) {
		std::rethrow_exception(failure);
	}
}
