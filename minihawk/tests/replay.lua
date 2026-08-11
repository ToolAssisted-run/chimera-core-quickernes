-- miniHawk Level B witness: replay a quickerNES .sol input sequence through
-- EmuHawk's frontend input pipeline and dump the final RAM (low mem, 2KB).
--
-- Job description is read from the file named by the MINIHAWK_JOB env var:
--   sol=<path to .sol>
--   out=<path to write final RAM dump (binary)>
--   meta=<path to write result metadata (text)>
--   controller1=Joypad|FourScore1|ArkanoidNES|ArkanoidFamicom|None
--   controller2=Joypad|FourScore2|None
--   mode=simple|rerecord
--
-- mode=simple   : set inputs, frameadvance, repeat (tester cycleType Simple)
-- mode=rerecord : load state, set inputs, frameadvance, save state, repeat
--                 (tester cycleType Rerecord; exercises IStatable every frame)

local function writeAll(path, data)
	local f = assert(io.open(path, "wb"))
	f:write(data)
	f:close()
end

local meta = {}
local function finish(status, detail)
	local lines = {
		"status=" .. status,
		"detail=" .. (detail or ""),
		"frames=" .. (meta.frames or 0),
		"lag=" .. (meta.lag or 0),
		"startframe=" .. (meta.startframe or -1),
		"boardname=" .. (meta.boardname or ""),
	}
	if meta.metaPath then
		writeAll(meta.metaPath, table.concat(lines, "\n") .. "\n")
	end
	client.exit()
end

-- ---------- read job ----------
local jobPath = os.getenv("MINIHAWK_JOB")
if jobPath == nil then
	error("MINIHAWK_JOB env var not set")
end
local job = {}
for line in io.lines(jobPath) do
	local k, v = line:match("^([^=]+)=(.*)$")
	if k then job[k] = v end
end
meta.metaPath = job.meta

-- ---------- read .sol ----------
local solLines = {}
do
	local f = assert(io.open(job.sol, "rb"))
	for line in f:lines() do
		line = line:gsub("\r$", "")
		if line:sub(1, 1) == "|" then solLines[#solLines + 1] = line end
	end
	f:close()
end

-- ---------- input decoding ----------
-- Console field: |Pr| -> Power / Reset
-- Joypad field: UDLRSsBA (Up Down Left Right Start Select B A), '.' = unpressed
local function decodeJoypad(s, prefix, buttons)
	local names = { "Up", "Down", "Left", "Right", "Start", "Select", "B", "A" }
	for i = 1, 8 do
		local c = s:sub(i, i)
		buttons[prefix .. " " .. names[i]] = (c ~= "." and c ~= "")
	end
end

-- Arkanoid field: " NNN,F" (3-char decimal potentiometer, comma, Fire)
local function decodeArkanoid(s)
	local potStr = s:sub(3, 5):gsub(" ", "")
	local pot = tonumber(potStr) or 0
	local fire = s:sub(7, 7) == "F"
	return pot, fire
end

-- Split a sol line on '|' into fields (fields exclude the outer separators)
local function splitFields(line)
	local fields = {}
	for field in line:gmatch("([^|]*)|") do
		fields[#fields + 1] = field
	end
	-- first field is empty (line starts with '|'); drop it
	table.remove(fields, 1)
	return fields
end

local c1 = job.controller1 or "Joypad"
local c2 = job.controller2 or "None"

-- Axis values cannot be delivered via joypad.setanalog (axis sticky-holds never
-- reach the output controller in this build), so the Arkanoid controller types
-- deliver the whole frame's input as a bk2 mnemonic string instead:
-- joypad.setfrommnemonicstr routes both buttons AND axes through the
-- ButtonOverrideAdapter, which Controller.Overrides() honors.
-- bk2 entry shape: groups by player (console group first), axes-before-buttons
-- within a group, axes rendered as PadLeft(5) .. ',', empty groups still emit
-- their '|' delimiter.
local function padLeft5(n)
	local s = tostring(n)
	return string.rep(" ", 5 - #s) .. s
end

local function applyInput(line)
	local fields = splitFields(line)
	local buttons = {}
	local fi = 1

	-- console field: parsed for structure but NOT applied — the native tester
	-- (our ground truth) parses reset/power flags and then ignores them during
	-- replay, so applying them here would diverge (seen with solarJetman's
	-- frame-3 reset).
	fi = fi + 1

	if c1 == "ArkanoidNES" then
		-- def groups: [0]=Reset,Power [1]=(empty) [2]=P2 Paddle,P2 Fire
		local pot, fire = decodeArkanoid(fields[fi])
		joypad.setfrommnemonicstr("|..||" .. padLeft5(pot) .. "," .. (fire and "F" or ".") .. "|")
		return
	elseif c1 == "ArkanoidFamicom" then
		-- def groups: [0]=Reset,Power [1]=P1 joypad [2]=P2 dummy (7 buttons) [3]=P3 Paddle,P3 Fire
		local joy = fields[fi]
		fi = fi + 2 -- skip the 7-dot unsupported famicom expansion field
		local pot, fire = decodeArkanoid(fields[fi])
		joypad.setfrommnemonicstr("|..|" .. joy .. "|.......|" .. padLeft5(pot) .. "," .. (fire and "F" or ".") .. "|")
		return
	end

	-- controller 1
	if c1 == "Joypad" then
		decodeJoypad(fields[fi], "P1", buttons)
		fi = fi + 1
	elseif c1 == "FourScore1" then
		decodeJoypad(fields[fi], "P1", buttons)
		decodeJoypad(fields[fi + 1], "P3", buttons)
		fi = fi + 2
	end

	-- controller 2
	if c2 == "Joypad" then
		decodeJoypad(fields[fi], "P2", buttons)
		fi = fi + 1
	elseif c2 == "FourScore2" then
		decodeJoypad(fields[fi], "P2", buttons)
		decodeJoypad(fields[fi + 1], "P4", buttons)
		fi = fi + 2
	end

	joypad.set(buttons)
end

-- ---------- sanity checks ----------
if emu.getsystemid() ~= "NES" then
	finish("ERROR", "wrong system id: " .. tostring(emu.getsystemid()))
end
meta.boardname = tostring(emu.getboardname())

-- ---------- clean power-on ----------
-- EmuHawk emulates one frame during ROM load, before this script gains
-- control; that would shift the whole replay by one frame. Reboot the core
-- to a fresh power-on so the input sequence starts at frame 0 exactly.
if emu.framecount() > 0 then
	client.reboot_core()
	if emu.framecount() > 0 then
		emu.frameadvance() -- let a pending async reboot apply
	end
end
meta.startframe = emu.framecount()
if meta.startframe ~= 0 then
	finish("ERROR", "could not reach clean frame 0 (framecount=" .. meta.startframe .. ")")
end

-- ---------- speed ----------
pcall(function() client.speedmode(6400) end)
pcall(function() client.invisibleemulation(true) end)

-- ---------- replay ----------
local rerecord = (job.mode == "rerecord")
local stateId = nil
if rerecord then
	stateId = memorysavestate.savecorestate()
end

local checkpointInterval = tonumber(job.checkpoint or "0") or 0
local checkpoints = {}

for i = 1, #solLines do
	if rerecord then
		memorysavestate.loadcorestate(stateId)
	end
	applyInput(solLines[i])
	emu.frameadvance()
	if rerecord then
		memorysavestate.removestate(stateId)
		stateId = memorysavestate.savecorestate()
	end
	if checkpointInterval > 0 and i % checkpointInterval == 0 then
		local cram = memory.read_bytes_as_array(0, 0x800, "RAM")
		local cchunks = {}
		for k = 1, #cram do
			cchunks[k] = string.char(cram[k])
		end
		checkpoints[#checkpoints + 1] = table.concat(cchunks)
	end
end

if checkpointInterval > 0 then
	writeAll(job.out .. ".ckpt", table.concat(checkpoints))
end

meta.frames = emu.framecount()
meta.lag = emu.lagcount()

-- ---------- dump final RAM ----------
local ram = memory.read_bytes_as_array(0, 0x800, "RAM")
local chunks = {}
for i = 1, #ram do
	chunks[i] = string.char(ram[i])
end
writeAll(job.out, table.concat(chunks))

finish("OK", "")
