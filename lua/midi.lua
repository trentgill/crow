--- midi
-- crow's USB MIDI behaviours. The channel picks the behaviour: midi.ch[n]
-- holds each channel's, and a jack belongs to whichever channel last drove it.
--
--   midi.ch[1] = midi.voice{ v8 = 1, gate = 2, env = 'adsr' }
--   midi.ch[2] = midi.cc{ [74] = 3, [1] = 4 }
--   midi.ch[16] = function(status, d1, d2) ... end -- a raw handler
--   midi.receive = function(status, d1, d2) ... end -- sees every message first
--   midi.send(0x90, 60, 100)

local M = { ch = {}, owner = {} }

M.send  = usb_midi_send
M.note0 = 60 -- the MIDI note at 0 V

local function ntov(n) return (n - M.note0) / 12 end

-- 0..127 -> 1ms..10s, evenly per decade
local function cc_time(v) return 0.001 * 10000^(v/127) end

-- a jack belongs to the behaviour that last drove it. taking one sets up its
-- action (once, so the notes that follow only touch dynamics)
local function take(b, o)
    if M.owner[o] ~= b then
        M.owner[o] = b
        b:claim(o)
    end
end

local function remove(t, n)
    for i=#t,1,-1 do if t[i] == n then table.remove(t, i) end end
end


--- voice: v8 + envelope, last-note priority
-- options: v8, gate, env ('trig' | 'adsr'), level (V at velocity 127),
--          attack, decay, sustain (V), release, bend_range (semitones), slew (v8),
--          legato (adsr: held notes don't retrigger), vel (an output for velocity),
--          cc (a table of CC number -> output)
local Voice = {}
Voice.__index = Voice

function M.voice(t)
    t = t or {}
    local v = { v8 = t.v8 or 1
              , gate = t.gate or 2
              , env = t.env or 'trig'
              , level = t.level or 8
              , attack = t.attack or 0.002
              , decay = t.decay or 0.2
              , sustain = t.sustain or 5
              , release = t.release or 0.3
              , bend_range = t.bend_range or 2
              , slew = t.slew or 0
              , legato = t.legato ~= false
              , vel = t.vel
              , ccmap = t.cc or {}
              , stack = {}
              , pb = 0
              }
    return setmetatable(v, Voice)
end

function Voice:claim(o)
    local out = output[o]
    if o == self.gate then
        if self.env == 'adsr' then
            out.action = { held{ to(dyn{peak=self.level}, dyn{a=self.attack}, 'log')
                               , to(dyn{s=self.sustain}, dyn{d=self.decay}, 'log')
                               }
                         , to(0, dyn{r=self.release}, 'log')
                         }
        else
            out.action = { to(dyn{peak=self.level}, 0, 'now')
                         , to(0, dyn{d=self.decay}, 'log')
                         }
        end
    else -- pitch, velocity & CC jacks just follow volts
        out.slew = (o == self.v8) and self.slew or 0.002
    end
end

function Voice:pitch()
    local n = self.stack[#self.stack]
    if not n then return end
    take(self, self.v8)
    output[self.v8].volts = ntov(n) + self.pb * self.bend_range / 12
end

function Voice:note_on(n, vel)
    local playing = #self.stack > 0
    remove(self.stack, n)
    table.insert(self.stack, n)
    self:pitch()
    if self.vel then
        take(self, self.vel)
        output[self.vel].volts = vel * 10 / 127
    end
    take(self, self.gate)
    local g = output[self.gate]
    g.dyn.peak = self.level * vel / 127
    if self.env == 'adsr' then
        if not (playing and self.legato) then g(true) end
    else
        g()
    end
end

function Voice:note_off(n)
    remove(self.stack, n)
    if #self.stack == 0 then
        if self.env == 'adsr' and M.owner[self.gate] == self then output[self.gate](false) end
    else
        self:pitch() -- back to the note still held
    end
end

-- update an envelope dynamic, if this voice has the jack
function Voice:set(k, v)
    self[k] = v
    if M.owner[self.gate] ~= self then return end
    local d = output[self.gate].dyn
    local name = ({attack='a', decay='d', sustain='s', release='r'})[k]
    if name and d._names[name] then d[name] = v end -- the trigger env has no a, s or r
end

function Voice:cc(num, val)
    local o = self.ccmap[num]
    if o then
        take(self, o)
        output[o].volts = val * 10 / 127
    elseif num == 73 then self:set('attack', cc_time(val))
    elseif num == 75 then self:set('decay', cc_time(val))
    elseif num == 79 then self:set('sustain', val * self.level / 127)
    elseif num == 72 then self:set('release', cc_time(val))
    elseif num == 123 or num == 120 then -- all notes / sound off
        self.stack = {}
        self:note_off(-1)
    end
end

function Voice:bend(v)
    self.pb = (v - 8192) / 8192
    self:pitch()
end


--- duo: two voices sharing a channel, round robin
local Duo = {}
Duo.__index = Duo

function M.duo(t)
    t = t or {}
    local a = M.voice{ v8 = t.v8 or 1, gate = t.gate or 2, env = t.env }
    local b = M.voice{ v8 = t.v8b or 3, gate = t.gateb or 4, env = t.env }
    return setmetatable({ voices = {a, b}, next = 1 }, Duo)
end

function Duo:note_on(n, vel)
    for _,v in ipairs(self.voices) do -- the same note again goes where it was
        if v.stack[1] == n then return v:note_on(n, vel) end
    end
    local pick = self.next
    for i,v in ipairs(self.voices) do -- prefer a voice that's free
        if #v.stack == 0 then pick = i; break end
    end
    self.next = (pick % #self.voices) + 1
    local v = self.voices[pick]
    v.stack = {} -- one note per voice
    v:note_on(n, vel)
end

function Duo:note_off(n)
    for _,v in ipairs(self.voices) do
        if v.stack[1] == n then v:note_off(n) end
    end
end

function Duo:cc(num, val) for _,v in ipairs(self.voices) do v:cc(num, val) end end
function Duo:bend(val) for _,v in ipairs(self.voices) do v:bend(val) end end


--- cc: CC numbers -> outputs
-- options: [ccnum] = output, range = {lo, hi} (V), slew (s)
local CC = {}
CC.__index = CC

function M.cc(t)
    t = t or {}
    local c = { map = {}, range = t.range or {0, 10}, slew = t.slew or 0.005 }
    for k,v in pairs(t) do
        if type(k) == 'number' then c.map[k] = v end
    end
    return setmetatable(c, CC)
end

function CC:claim(o) output[o].slew = self.slew end

function CC:cc(num, val)
    local o = self.map[num]
    if not o then return end
    take(self, o)
    local lo, hi = self.range[1], self.range[2]
    output[o].volts = lo + (hi - lo) * val / 127
end


--- drums: notes -> triggers
-- options: [note] = output, length (s), level (V at velocity 127)
local Drums = {}
Drums.__index = Drums

function M.drums(t)
    t = t or {}
    local d = { map = {}, length = t.length or 0.01, level = t.level or 10 }
    for k,v in pairs(t) do
        if type(k) == 'number' then d.map[k] = v end
    end
    if next(d.map) == nil then d.map = {[36]=1, [38]=2, [42]=3, [46]=4} end -- GM kick, snare, hats
    return setmetatable(d, Drums)
end

function Drums:claim(o)
    output[o].action = { to(dyn{peak=self.level}, self.length, 'now'), to(0, 0) }
end

function Drums:note_on(n, vel)
    local o = self.map[n]
    if not o then return end
    take(self, o)
    output[o].dyn.peak = self.level * vel / 127
    output[o]()
end


--- ii: notes to Just Friends or W/Syn
-- options: level (V at velocity 127), off (send level 0 on note-off: for
-- JF's sustain mode)
local II = {}
II.__index = II

local function ii_voice(dev, t)
    t = t or {}
    return setmetatable({ dev = dev, level = t.level or 5, off = t.off, ready = false }, II)
end
function M.jf(t) return ii_voice('jf', t) end
function M.wsyn(t) return ii_voice('wsyn', t) end

function II:note_on(n, vel)
    local m = ii[self.dev]
    if not self.ready and self.dev == 'jf' then m.mode(1) end -- synthesis mode
    self.ready = true
    m.play_note(ntov(n), self.level * vel / 127)
end

function II:note_off(n)
    if self.off then ii[self.dev].play_note(ntov(n), 0) end
end


--- clock: MIDI clock & transport to jacks. realtime messages have no channel,
-- so there's one of these, in midi.clk
-- options: out (pulse jack), div (MIDI clocks per pulse: 6 = 16ths, 24 =
--          quarters), length (s), level (V), run (jack), run_mode ('gate': high
--          while playing | 'trig': a pulse on start), free (pulse while stopped)
local Clock = {}
Clock.__index = Clock

function M.clock(t)
    if not t then M.clk = nil; return end
    local c = { out = t.out, div = t.div or 6, length = t.length or 0.005, level = t.level or 5
              , run = t.run, run_mode = t.run_mode or 'gate', free = t.free
              , count = 0, running = false }
    M.clk = setmetatable(c, Clock)
    return M.clk
end

function Clock:claim(o)
    if o == self.out or self.run_mode == 'trig' then
        output[o].action = { to(self.level, self.length, 'now'), to(0, 0) }
    end
end

function Clock:tick()
    if not (self.running or self.free) then return end
    if self.out and self.count % self.div == 0 then
        take(self, self.out)
        output[self.out]()
    end
    self.count = self.count + 1
end

function Clock:transport(play, from_start)
    if from_start then self.count = 0 end -- divisions line up with the bar
    self.running = play
    if not self.run then return end
    take(self, self.run)
    if self.run_mode == 'trig' then
        if play and from_start then output[self.run]() end
    else
        output[self.run].volts = play and self.level or 0
    end
end

local realtime = { [0xF8] = function(c) c:tick() end
                 , [0xFA] = function(c) c:transport(true, true) end  -- start
                 , [0xFB] = function(c) c:transport(true, false) end -- continue
                 , [0xFC] = function(c) c:transport(false) end       -- stop
                 }


--- capture: a v8 + gate input pair -> notes to the host
-- options: v8 (input), gate (input), ch (1-16), delay (s: lets the v8 settle
--          after the gate), velocity, threshold (V)
function M.capture(t)
    if M.cap then input[M.cap.gate].mode('none') end
    M.cap = nil
    if not t then return end
    local c = { v8 = t.v8 or 1, gate = t.gate or 2, ch = t.ch or 1, delay = t.delay or 0.002
              , velocity = t.velocity or 100, held = nil }
    M.cap = c
    local st = c.ch - 1
    input[c.gate].mode('change', t.threshold or 1, 0.1, 'both')
    input[c.gate].change = function(high)
        if high then
            clock.run(function()
                if c.delay > 0 then clock.sleep(c.delay) end
                if M.cap ~= c then return end -- turned off meanwhile
                local n = math.floor(input[c.v8].volts * 12 + 0.5) + M.note0
                n = math.max(0, math.min(127, n))
                if c.held then M.send(0x80 + st, c.held, 0) end
                M.send(0x90 + st, n, c.velocity)
                c.held = n
            end)
        elseif c.held then
            M.send(0x80 + st, c.held, 0)
            c.held = nil
        end
    end
    return c
end

--- cv_cc: an input -> a CC to the host, sent when its 7-bit value changes
-- options: input, ch (1-16), cc, rate (s between samples), range = {lo, hi} (V)
M.cvcc = {}
function M.cv_cc(t)
    if not t or not t.cc then -- off: midi.cv_cc() for all, midi.cv_cc{input=n} for one
        for n,_ in pairs(M.cvcc) do
            if not t or t.input == n then input[n].mode('none'); M.cvcc[n] = nil end
        end
        return
    end
    local n = t.input or 1
    local c = { ch = t.ch or 1, cc = t.cc, range = t.range or {0, 10}, last = -1 }
    M.cvcc[n] = c
    input[n].mode('stream', t.rate or 0.01)
    input[n].stream = function(v)
        local lo, hi = c.range[1], c.range[2]
        local f = (v - lo) / (hi - lo) * 127
        -- a step of hysteresis: a still input that sits on a boundary stays quiet
        if f > c.last + 0.75 or f < c.last - 0.75 then
            local val = math.max(0, math.min(127, math.floor(f + 0.5)))
            if val ~= c.last then
                c.last = val
                M.send(0xB0 + c.ch - 1, c.cc, val)
            end
        end
    end
    return c
end


--- config: channel 16. ordinary CCs, NRPNs & program changes, so a DAW
-- project or a .mid file can set crow up (no sysex)
local Config = {}
Config.__index = Config

-- NRPN 1/<channel> = behaviour id, NRPN 2/<channel> = first jack
local function fit(j, n) return math.max(1, math.min(j, 5 - n)) end -- n jacks from j
M.kinds = { function()  return nil end                                          -- 0 none
          , function(j) j = fit(j, 2); return M.voice{ v8 = j, gate = j+1 } end -- 1 trigger voice
          , function(j) j = fit(j, 2); return M.voice{ v8 = j, gate = j+1, env = 'adsr' } end -- 2 ADSR
          , function(j) local t = {} for o=j,4 do t[16+o-j] = o end return M.cc(t) end -- 3 CCs
          , function() return M.voice{ v8 = 1, gate = 2, vel = 3, cc = {[1] = 4} } end -- 4 voice + CCs
          , function() return M.duo{} end                                       -- 5 duo
          , function() return M.jf{} end                                        -- 6 Just Friends
          , function() return M.wsyn{} end                                      -- 7 W/Syn
          , function(j) j = fit(j, 4) return M.drums{ [36] = j, [38] = j+1, [42] = j+2, [46] = j+3 } end -- 8
          }
M.jack = {} -- first jack per channel, as NRPN 2 set it

function M.config()
    return setmetatable({ clock = {}, cap = { delay = 0.002 }, cv = { ch = 1, rate = 0.01 }
                        , nrpn = {0, 0} }, Config)
end

local function clock_jack(v) return (v >= 1 and v <= 4) and v or nil end

function Config:clock_update()
    local k = self.clock
    if k.out or k.run then
        M.clock{ out = k.out, div = k.div or 6, run = k.run, run_mode = k.mode }
    else
        M.clock()
    end
end

function Config:cv_update(n, ccnum)
    if ccnum and ccnum > 0 then
        M.cv_cc{ input = n, ch = self.cv.ch, cc = ccnum, rate = self.cv.rate, range = self.cv.range }
    else
        M.cv_cc{ input = n }
    end
    self.cv[n] = ccnum
end

function Config:cc(num, val)
    local k = self.clock
    if     num == 20 then k.out = clock_jack(val); self:clock_update()
    elseif num == 21 then k.div = math.max(1, val); if M.clk then M.clk.div = k.div end
    elseif num == 22 then k.run = clock_jack(val); self:clock_update()
    elseif num == 23 then k.mode = (val >= 64) and 'trig' or 'gate'; self:clock_update()
    elseif num == 24 then
        if val >= 1 and val <= 16 then
            M.capture{ ch = val, delay = self.cap.delay }
        else M.capture() end
    elseif num == 25 then
        self.cap.delay = val / 1000
        if M.cap then M.cap.delay = self.cap.delay end
    elseif num == 26 or num == 27 then self:cv_update(num - 25, val)
    elseif num == 28 or num == 29 then
        if num == 28 then self.cv.ch = math.max(1, math.min(16, val))
        else self.cv.rate = math.max(1, val) / 100 end
        for n=1,2 do if self.cv[n] then self:cv_update(n, self.cv[n]) end end
    elseif num == 99 then self.nrpn[1] = val -- NRPN parameter MSB
    elseif num == 98 then self.nrpn[2] = val -- ..LSB: the target channel
    elseif num == 6 then -- data entry MSB
        local p, ch = self.nrpn[1], self.nrpn[2]
        if ch < 1 or ch > 15 then return end -- 16 stays the config channel
        local function assign(kind)
            local b = M.kinds[kind + 1](M.jack[ch] or 1)
            if b then b.kind = kind end
            M.ch[ch] = b
        end
        if p == 1 and M.kinds[val + 1] then
            assign(val)
        elseif p == 2 and val >= 1 and val <= 4 then
            M.jack[ch] = val
            local b = M.ch[ch]
            if type(b) == 'table' and b.kind then assign(b.kind) end
        end
    end
end

function Config:program(p)
    if p == 127 then usb.audio(true)
    elseif p == 126 then usb.audio(false)
    elseif p == 120 then M.reset() end
end


--- the default channel map: live at boot and after midi.reset()
function M.defaults()
    M.ch = {}
    M.ch[1]  = M.voice{ v8 = 1, gate = 2 }
    M.ch[2]  = M.voice{ v8 = 3, gate = 4 }
    M.ch[3]  = M.voice{ v8 = 1, gate = 2, env = 'adsr' }
    M.ch[4]  = M.cc{ [16] = 1, [17] = 2, [18] = 3, [19] = 4 }
    M.ch[5]  = M.voice{ v8 = 1, gate = 2, vel = 3, cc = {[1] = 4} }
    M.ch[6]  = M.duo{}
    M.ch[7]  = M.jf{}
    M.ch[8]  = M.wsyn{}
    M.ch[10] = M.drums{}
    M.ch[16] = M.config()
end

function M.reset()
    M.receive = nil
    M.clk = nil
    M.capture()
    M.cv_cc()
    M.owner = {}
    M.jack = {}
    M.defaults()
end


--- dispatch: called by crow for every message from the host
function midi_handler(st, d1, d2)
    if M.receive then M.receive(st, d1, d2) end
    if st >= 0xF0 then -- system & realtime: no channel
        local f = realtime[st]
        if f and M.clk then f(M.clk) end
        return
    end
    local b = M.ch[(st & 0x0F) + 1]
    if not b then return end
    if type(b) == 'function' then return b(st, d1, d2) end
    local kind = st & 0xF0
    if kind == 0x90 and d2 > 0 then
        if b.note_on then b:note_on(d1, d2) end
    elseif kind == 0x80 or kind == 0x90 then
        if b.note_off then b:note_off(d1) end
    elseif kind == 0xB0 then
        if b.cc then b:cc(d1, d2) end
    elseif kind == 0xE0 then
        if b.bend then b:bend(d1 | (d2 << 7)) end
    elseif kind == 0xC0 then
        if b.program then b:program(d1) end
    end
end

M.defaults()

return M
