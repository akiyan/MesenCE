-- Checks the table of nmi_boundary_rom.py's ROM:
--   Mesen --testrunner nmi-boundary.sfc nmi_boundary.lua
-- Stop code 0: the table is the expected one. 1: it differs (printed with emu.log). 2: the ROM did not finish.
-- For each field, the 96 tests: where the NMI was taken ('-' before the sequence, 0 before its first STA,
-- 2 after it, 4 after the LDA, 7 after the AND, 9 after the last STA, A-F in the NOPs after it, '+' later),
-- and whether bit 3 of the signal word survived (1) or was lost (0). It must be lost exactly at 4 and 7.
local SEQ = 0xB80F            -- the sequence's address (nmi_boundary_rom.py prints it)
local TESTS, RESULTS = 96, 0x1000
local EXPECTED = {            -- MesenCE 2.2.1, 2026-11-26
	[0] = "AAAA99999999999999999997777777777774444444444444444444222222222222222222000000000000------------",
	[1] = "AAAAA9A99999999999999999997777777777774444444444444444444222222222222222222000000000000---------",
}
local frames = 0

local function where(offset)
	if offset < 0 then return "-" end
	if offset >= 16 then return "+" end
	return string.format("%X", offset)
end

emu.addEventCallback(function()
	frames = frames + 1
	local ram = emu.memType.snesWorkRam
	if emu.read16(0x1C, ram, false) ~= 1 then
		if frames > 3000 then emu.log("the test ROM did not finish"); emu.stop(2) end
		return
	end
	local failed = false
	for field = 0, 1 do
		local taken, kept = {}, {}
		for test = 0, TESTS - 1 do
			local at = RESULTS + 4 * (TESTS * field + test)
			local offset = emu.read16(at, ram, false) - SEQ
			local word = emu.read16(at + 2, ram, false)
			local lost = (word & 8) == 0
			taken[#taken + 1] = where(offset)
			kept[#kept + 1] = lost and "0" or "1"
			if (offset == 4 or offset == 7) ~= lost then failed = true end
		end
		local line = table.concat(taken)
		emu.log(string.format("field %d taken %s", field, line))
		emu.log(string.format("field %d bit 3 %s", field, table.concat(kept)))
		if line ~= EXPECTED[field] then
			emu.log(string.format("field %d expected %s", field, EXPECTED[field]))
			failed = true
		end
	end
	emu.stop(failed and 1 or 0)
end, emu.eventType.endFrame)
