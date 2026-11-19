local output = assert(os.getenv("MESEN_HEADLESS_TEST_EVIDENCE"))
local stage = "initial"
local debuggerSample, eventCount, savedState

local function checked(callback)
	return function()
		local ok, message = pcall(callback)
		if not ok then
			io.stderr:write(tostring(message) .. "\n")
			emu.stop(40)
		end
	end
end

local function findEvent(trace, kind, access, address, value, channel)
	for index, event in ipairs(trace.events) do
		if event.kind == kind and event.access == access and event.address == address
			and event.value == value and event.channel == channel then
			return index
		end
	end
	error(string.format("missing %s %s event for %s", kind, tostring(access), tostring(address)))
end

emu.enableSnesFrameCapture(true)
assert(not emu.getSnesPpuSnapshot().completedFrame)
assert(not pcall(emu.getSnesPpuSnapshot, 0x20000, 1))
assert(not pcall(emu.startSnesPpuTrace, 65537))
emu.startSnesPpuTrace(4096)

emu.addEventCallback(checked(function()
	if stage == "initial" then
		local sample = emu.getSnesPpuSnapshot(0x10, 2)
		if sample.live.frame < 2 then
			return
		end
		assert(sample.live.boundary == "vblankStart", sample.live.boundary)
		assert(sample.cpu.pc >= 0x8000 and sample.cpu.cycleCount > 0)
		assert(sample.ppu.bgMode == 0 and #sample.ppu.layers == 4)
		assert(#sample.ppu.palette == 256 and sample.ppu.palette[1] == 0x03E0)
		assert(sample.wram.offset == 0x10 and #sample.wram.bytes == 2)
		assert(sample.wram.bytes[1] > 0 and sample.wram.bytes[2] > 0)
		assert(sample.completedFrame and sample.completedFrame.masterClock < sample.live.masterClock)

		local trace = emu.stopSnesPpuTrace()
		assert(trace.dropped == 0, "unexpected dropped events: " .. tostring(trace.dropped))
		local blank = findEvent(trace, "ppu", "write", 0x2100, 0x80)
		local red = findEvent(trace, "ppu", "write", 0x2122, 0x1F)
		local dmaRead = findEvent(trace, "dma", "read", 0x9000, 0xE0, 0)
		local dmaWrite = findEvent(trace, "dma", "write", 0x2122, 0xE0, 0)
		local hdmaTable = findEvent(trace, "hdma", "read", 0x9004, 2, 1)
		local hdmaWrite = findEvent(trace, "hdma", "write", 0x2100, 0x0A, 1)
		assert(blank < red and red < dmaRead and dmaRead < dmaWrite)
		assert(dmaWrite < hdmaTable and hdmaTable < hdmaWrite)
		findEvent(trace, "irq")
		findEvent(trace, "nmi")
		local lastClock = 0
		local timeline = assert(io.open(output .. "/events.tsv", "w"))
		for _, event in ipairs(trace.events) do
			assert(event.masterClock >= lastClock)
			assert(event.scanline >= 0 and event.scanline < 262)
			assert(event.cycle >= 0 and event.cycle < 1364)
			assert(timeline:write(string.format("%d\t%d\t%d\t%d\t%s\t%s\t%d\t%d\t%d\n",
				event.frame, event.masterClock, event.scanline, event.cycle, event.kind, event.access or "",
				event.address or -1, event.value or -1, event.channel or -1)))
			lastClock = event.masterClock
		end
		assert(timeline:close())
		eventCount = #trace.events
		stage = "stepping"
		emu.step(1, emu.stepType.ppuScanline)
	elseif stage == "bounded" then
		local bounded = emu.stopSnesPpuTrace()
		assert(#bounded.events == 1 and bounded.dropped > 0)
		local image = debuggerSample.completedFrame
		local png = assert(io.open(output .. "/completed.png", "wb"))
		assert(png:write(image.png))
		assert(png:close())
		local report = assert(io.open(output .. "/raster.json", "w"))
		assert(report:write(string.format(
			'{"width":%d,"height":%d,"completedClock":%d,"debuggerClock":%d,"palette0":%d,"events":%d,"dropped":%d}\n',
			image.width, image.height, image.masterClock, debuggerSample.live.masterClock,
			debuggerSample.ppu.palette[1], eventCount, bounded.dropped)))
		assert(report:close())
		emu.startSnesPpuTrace(16)
		stage = "state-load"
	end
end), emu.eventType.endFrame)

emu.addEventCallback(checked(function()
	assert(stage == "stepping", stage)
	debuggerSample = emu.getSnesPpuSnapshot()
	assert(debuggerSample.live.boundary == "debugger", debuggerSample.live.boundary)
	assert(debuggerSample.completedFrame and debuggerSample.completedFrame.png)
	assert(debuggerSample.completedFrame.masterClock < debuggerSample.live.masterClock)
	assert(debuggerSample.completedFrame.scanline == 225)
	emu.startSnesPpuTrace(1)
	stage = "bounded"
	emu.resume()
end), emu.eventType.codeBreak)

emu.addMemoryCallback(checked(function()
	if not savedState then
		savedState = emu.createSavestate()
	elseif stage == "state-load" then
		assert(emu.getSnesPpuSnapshot().completedFrame)
		assert(emu.loadSavestate(savedState), "could not restore the fixture's state")
		assert(not emu.getSnesPpuSnapshot().completedFrame, "state restoration retained the previous image")
		local trace = emu.stopSnesPpuTrace()
		assert(#trace.events == 0 and trace.dropped == 0, "state restoration retained the previous timeline")
		emu.enableSnesFrameCapture(false)
		emu.stop(0)
	end
end), emu.callbackType.exec, 0x008100, 0x008100, emu.cpuType.snes, emu.memType.snesMemory)
