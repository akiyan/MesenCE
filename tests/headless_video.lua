-- Writes every frame's raw screen bytes and checks them against getScreenBuffer.
local output = assert(os.getenv("MESEN_HEADLESS_TEST_EVIDENCE"))
local video = assert(io.open(output .. "/frames.bgr0", "wb"))
local sizes = {}
local frame = 0
local FRAMES = 32

emu.addEventCallback(function()
	frame = frame + 1
	if frame > FRAMES then return end
	local bytes, width, height = emu.getScreenBytes()
	local pixels = emu.getScreenBuffer()
	local size = emu.getScreenSize()
	if #pixels ~= width * height or #bytes ~= #pixels * 4 or size.width ~= width or size.height ~= height then
		emu.stop(2)
		return
	end
	for i = 1, #pixels do
		if string.unpack("<I4", bytes, 4 * i - 3) ~= pixels[i] then
			emu.stop(3)
			return
		end
	end
	assert(video:write(bytes))
	sizes[frame] = string.format("[%d,%d]", width, height)
	if frame == FRAMES then
		assert(video:close())
		local file = assert(io.open(output .. "/sizes.json", "w"))
		assert(file:write("[" .. table.concat(sizes, ",") .. "]\n"))
		assert(file:close())
		emu.stop(0)
	end
end, emu.eventType.endFrame)
