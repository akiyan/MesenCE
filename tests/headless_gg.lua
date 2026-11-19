local output = assert(os.getenv("MESEN_HEADLESS_TEST_EVIDENCE"))
local frame = 0
local ram = emu.memType.smsWorkRam

local function bytes(address, count)
	local values = {}
	for index = 0, count - 1 do
		values[#values + 1] = tostring(emu.read(address + index, ram))
	end
	return table.concat(values, ",")
end

local function screenshot(name)
	local file = assert(io.open(output .. "/" .. name .. ".png", "wb"))
	assert(file:write(emu.takeScreenshot()))
	assert(file:close())
end

emu.addEventCallback(function()
	frame = frame + 1
end, emu.eventType.startFrame)

emu.addEventCallback(function()
	emu.setInput({ one = frame == 15, pause = frame == 17 }, 0)
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
	if frame == 21 then
		screenshot("first")
	elseif frame == 22 then
		screenshot("second")
		local observation = assert(io.open(output .. "/observation.json", "w"))
		observation:write(string.format('{"frame":%d,"count":%d,"pads":[%s],"starts":[%s],"zero":[%s]}\n',
			frame, emu.read(0x10, ram), bytes(0x20, 16), bytes(0x40, 16), bytes(0x80, 16)))
		assert(observation:close())
		emu.stop(0)
	end
end, emu.eventType.endFrame)
