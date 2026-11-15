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


--- the default channel map: live at boot and after midi.reset()
function M.defaults()
    M.ch = {}
    M.ch[1]  = M.voice{ v8 = 1, gate = 2 }
    M.ch[2]  = M.voice{ v8 = 3, gate = 4 }
    M.ch[3]  = M.voice{ v8 = 1, gate = 2, env = 'adsr' }
    M.ch[4]  = M.cc{ [16] = 1, [17] = 2, [18] = 3, [19] = 4 }
    M.ch[5]  = M.voice{ v8 = 1, gate = 2, vel = 3, cc = {[1] = 4} }
    M.ch[6]  = M.duo{}
    M.ch[10] = M.drums{}
end

function M.reset()
    M.receive = nil
    M.owner = {}
    M.defaults()
end


--- dispatch: called by crow for every message from the host
function midi_handler(st, d1, d2)
    if M.receive then M.receive(st, d1, d2) end
    if st >= 0xF0 then return end -- system & realtime: no channel
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
    end
end

M.defaults()

return M
