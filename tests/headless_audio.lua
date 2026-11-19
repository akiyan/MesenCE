assert(emu.getSnesAudioCapture().state == "idle")
assert(emu.startSnesAudioCapture("capture.wav", "events.jsonl", 8))
assert(emu.getSnesAudioCapture().state == "armed")

emu.addEventCallback(function()
	local capture = emu.getSnesAudioCapture()
	if capture.state == "error" then
		emu.stop(9)
	elseif capture.state == "complete" then
		local file = assert(io.open("result.json", "w"))
		assert(file:write(string.format(
			'{"frames":%d,"samples":%d,"events":%d,"droppedEvents":%d,"startClock":%d,"endClock":%d,"clockRate":%d,"sampleRate":%d}\n',
			capture.frames, capture.samples, capture.events, capture.droppedEvents,
			capture.startClock, capture.endClock, capture.clockRate, capture.sampleRate)))
		assert(file:close())
		emu.stop(0)
	end
end, emu.eventType.startFrame)
