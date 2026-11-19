local output = assert(os.getenv("MESEN_HEADLESS_TEST_EVIDENCE"))
local first, last = tonumber(os.getenv("SCREENSHOT_FIRST")), tonumber(os.getenv("SCREENSHOT_LAST"))
local frame = 0
local log = assert(io.open(output .. "/frames.tsv", "w"))

-- At the end of every frame: the PNG of emu.takeScreenshot() and the middle pixel of emu.getScreenBytes().
emu.addEventCallback(function()
	frame = frame + 1
	if frame >= first then
		local file = assert(io.open(string.format("%s/shot-%04d.png", output, frame), "wb"))
		assert(file:write(emu.takeScreenshot()))
		assert(file:close())
		local bytes, width, height = emu.getScreenBytes()
		local at = ((height // 2) * width + width // 2) * 4
		assert(log:write(string.format("%d\t%02x%02x%02x\n", frame, bytes:byte(at + 3), bytes:byte(at + 2), bytes:byte(at + 1))))
	end
	if frame == last then
		assert(log:close())
		emu.stop(0)
	end
end, emu.eventType.endFrame)
