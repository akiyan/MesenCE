local output = assert(os.getenv("MESEN_HEADLESS_TEST_EVIDENCE"))
local frame = 0
local finished = false
local wram = emu.memType.snesWorkRam

emu.addEventCallback(function()
	frame = frame + 1
end, emu.eventType.startFrame)

emu.addEventCallback(function()
	emu.setInput({ b = frame == 2 or frame == 3 }, 0)
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
	if finished then
		return
	end
	local polls = emu.read(0x11, wram)
	if polls == 5 then
		finished = true
		local samples = {}
		for index = 0, polls - 1 do
			samples[#samples + 1] = tostring(emu.read(0x20 + index, wram))
		end
		local observation = assert(io.open(output .. "/observation.json", "w"))
		observation:write(string.format(
			'{"frame":%d,"masterClock":%d,"pressed":%d,"polls":%d,"samples":[%s]}\n',
			frame, emu.getMasterClock(), emu.read(0x10, wram), polls, table.concat(samples, ",")))
		assert(observation:close())
		local screenshot = assert(io.open(output .. "/screen.png", "wb"))
		assert(screenshot:write(emu.takeScreenshot()))
		assert(screenshot:close())
		emu.stop(0)
	elseif frame >= 12 then
		emu.stop(10)
	end
end, emu.eventType.endFrame)
