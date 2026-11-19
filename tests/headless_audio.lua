assert(emu.getSnesAudioCapture().state == "idle")
-- MESEN_AUDIO_STOPPED records without events and stops the open-ended capture after 8 frames.
local stopped = os.getenv("MESEN_AUDIO_STOPPED") == "1"
local requested = false
if stopped then
	local ok, failure = emu.stopAudioCapture()
	assert(ok == nil and failure)
	assert(not pcall(emu.startAudioCapture, "capture.wav", nil, 1080001))
	assert(emu.startAudioCapture("capture.wav", nil, 1080000))
else
	assert(emu.startSnesAudioCapture("capture.wav", "events.jsonl", 8))
end
assert(emu.getSnesAudioCapture().state == "armed")

emu.addEventCallback(function()
	local capture = emu.getSnesAudioCapture()
	if capture.state == "error" then
		emu.stop(9)
	elseif stopped and not requested and capture.state == "recording" and capture.frames == 7 then
		requested = true
		assert(emu.stopAudioCapture())
	elseif capture.state == "complete" then
		if stopped then
			assert(requested and emu.stopAudioCapture() == nil)
		end
		local file = assert(io.open("result.json", "w"))
		assert(file:write(string.format(
			'{"frames":%d,"samples":%d,"events":%d,"droppedEvents":%d,"startClock":%d,"endClock":%d,"clockRate":%d,"sampleRate":%d}\n',
			capture.frames, capture.samples, capture.events, capture.droppedEvents,
			capture.startClock, capture.endClock, capture.clockRate, capture.sampleRate)))
		assert(file:close())
		emu.stop(0)
	end
end, emu.eventType.startFrame)
