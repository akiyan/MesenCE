-- Records 8 frames of a generated Game Gear ROM's audio after frame 4 and counts
-- the PSG and stereo port writes the Lua port callbacks see.
local output = assert(os.getenv("MESEN_HEADLESS_TEST_EVIDENCE"))
local frame, psg, stereo, second, done = 0, 0, -1, nil, false
-- MESEN_GG_AUDIO_FIRST moves the recording (the wrap test starts it before frame 65,536).
local first = tonumber(os.getenv("MESEN_GG_AUDIO_FIRST") or "4")

emu.addMemoryCallback(function(address, value)
	psg = psg + 1
end, emu.callbackType.write, 0x40, 0x7F, emu.cpuType.sms, emu.memType.smsPort)
emu.addMemoryCallback(function(address, value)
	stereo = value
end, emu.callbackType.write, 0x06, 0x06, emu.cpuType.sms, emu.memType.smsPort)

emu.addEventCallback(function()
	frame = frame + 1
	if frame == first then
		assert(emu.startAudioCapture(output .. "/mix.wav", output .. "/psg.jsonl", 8))
		local ok, failure = emu.startAudioCapture(output .. "/other.wav", output .. "/other.jsonl", 8)
		second = ok == nil and failure or "accepted"
	end
	local capture = emu.getAudioCapture()
	-- emu.stop takes effect later: write the status once.
	if not done and (capture.state == "complete" or capture.state == "error") then
		done = true
		local snes = emu.getSnesAudioCapture()
		local file = assert(io.open(output .. "/status.json", "w"))
		file:write(string.format(
			'{"state":"%s","frames":%d,"samples":%d,"events":%d,"droppedEvents":%d,"startClock":%d,"endClock":%d,' ..
			'"clockRate":%d,"sampleRate":%d,"psgWrites":%d,"stereo":%d,"second":"%s","alias":"%s","frame":%d}\n',
			capture.state, capture.frames, capture.samples, capture.events, capture.droppedEvents,
			capture.startClock, capture.endClock, capture.clockRate, capture.sampleRate, psg, stereo, second,
			snes.state, frame))
		assert(file:close())
		emu.stop(capture.state == "complete" and 0 or 1)
	end
end, emu.eventType.endFrame)
