-- crow-to-crow ii test. loaded on both crows; the host calls setup() then run()
-- every message carries a sequence number and a checksum, every query answer is
-- a known function of its argument, so the receiving side can count what went wrong

local function ck(s, a, b) return (s*7 + a*3 + b) % 32749 end
local function qf(x) return (x*97 + 11) % 30000 end

peer = 2
function setup(me, other)
    ii.set_address(me)
    peer = other
    rxreset()
end

-- follower side
function rxreset()
    R = {n=0, gap=0, dup=0, bad=0, ooo=0, last=-1, q=0}
end
rxreset()
ii.self.call4 = function(s, a, b, c)
    R.n = R.n + 1
    if c ~= ck(s, a, b) then R.bad = R.bad + 1 end
    if R.last >= 0 then
        local want = (R.last + 1) % 32768
        if s == R.last then R.dup = R.dup + 1
        elseif s ~= want then
            local d = (s - want) % 32768
            if d < 16384 then R.gap = R.gap + d else R.ooo = R.ooo + 1 end
        end
    end
    R.last = s
end
ii.self.query1 = function(x)
    R.q = R.q + 1
    return qf(x)
end

-- leader side
T = {}
ii.crow.event = function(e, v)
    if e.name == 'query1' then
        if v == qf(e.arg) then T.qok = T.qok + 1 else T.qbad = T.qbad + 1 end
    else
        T.other = T.other + 1
    end
end

-- n ticks of `period` seconds; each tick sends `per` messages of kind
-- 'call', 'query' or 'mix' (alternating)
function run(n, period, per, kind)
    T = {sent=0, qsent=0, qok=0, qbad=0, other=0, seq=0, cseq=0, done=false}
    if M then metro.free(M.id) end
    M = metro.init{ time = period, count = n, event = function(c)
        for i = 1, per do
            local s = T.seq
            T.seq = (s + 1) % 32768
            local q = (kind == 'query') or (kind == 'mix' and s % 2 == 1)
            if q then
                ii.crow[peer].get('query1', s % 256)
                T.qsent = T.qsent + 1
            else
                s = T.cseq -- calls carry their own unbroken sequence
                T.cseq = (s + 1) % 32768
                local a, b = (s*13) % 32768, (s*29) % 32768
                ii.crow[peer].call4(s, a, b, ck(s, a, b))
                T.sent = T.sent + 1
            end
        end
        if c == n then T.done = true end
    end }
    M:start()
end

function report()
    local s = ii.stats()
    local out = {}
    for k, v in pairs(s) do out[#out+1] = k .. '=' .. v end
    for k, v in pairs(T) do out[#out+1] = 'T.' .. k .. '=' .. tostring(v) end
    for k, v in pairs(R) do out[#out+1] = 'R.' .. k .. '=' .. tostring(v) end
    print('REPORT ' .. table.concat(out, ' '))
end

-- W/ (Tape, ii address 0x71) as the follower. 'wq' queries speed and checks
-- every answer against the expected value; 'wset' sets speed to a changing
-- value then queries it, checking each answer against what was set (in order).
W = {}
ii.wtape.event = function(e, v)
    if e.name ~= 'speed' then return end
    local want = W.fifo and table.remove(W.fifo, 1) or W.want
    -- W/ applies a new speed on its next audio block (1.33 ms), so a query
    -- straight after a set may still see the one before it
    local prev = W.prev
    if W.fifo then W.prev = want end
    if want and math.abs(v - want) < 0.002 then W.ok = W.ok + 1
    elseif prev and math.abs(v - prev) < 0.002 then W.ok = W.ok + 1; W.late = (W.late or 0) + 1
    else W.bad = W.bad + 1; W.last_bad = v end
end
function runw(n, period, per, kind, want)
    W = {sent=0, qsent=0, ok=0, bad=0, seq=0, want=want, fifo=(kind == 'wset') and {} or nil}
    if kind == 'wset' then W.prev = want; W.want = nil end -- the speed before the first set
    -- swallow the answers to the bad getters before Lua looks them up
    ii.event_raw = function(addr, cmd) return addr == 0x71 and cmd ~= 132 end
    if MW then metro.free(MW.id) end
    MW = metro.init{ time = period, count = n, event = function(c)
        for i = 1, per do
            local s = W.seq
            W.seq = s + 1
            if kind == 'wset' then
                local v = 0.5 + (s % 8) * 0.125
                ii.wtape.speed(v)
                W.sent = W.sent + 1
                table.insert(W.fifo, v)
            end
            if kind == 'wbad' and s % 2 == 0 then
                -- getters W/ answers with 0 bytes: null, loop_next, skip, unknown
                local bad = {128, 142, 144, 200}
                ii.raw(0x71, string.char(bad[(s // 2) % 4 + 1]), 2)
                W.sent = W.sent + 1
            end
            ii.wtape.get('speed')
            W.qsent = W.qsent + 1
        end
    end }
    MW:start()
end
function reportw()
    local s = ii.stats()
    local out = {}
    for k, v in pairs(s) do out[#out+1] = k .. '=' .. v end
    for k, v in pairs(W) do if type(v) ~= 'table' then out[#out+1] = 'W.' .. k .. '=' .. tostring(v) end end
    print('REPORT ' .. table.concat(out, ' '))
end

-- Just Friends (ii address 0x70) as the follower. 'jq' alternates the speed
-- switch getter (1 byte) and run (2 bytes) and checks every answer; 'jset'
-- sets run to a changing value then queries it, checking each answer against
-- what was set (in order); 'jnote'/'jvox' send note setters (5 and 6 bytes)
-- with a run query between; 'jlong' sends run setters padded to 9 bytes;
-- 'jbad' asks for getters JF has no answer for between good queries.
-- Values are raw s16V: on JF's original driver the high byte of a 2-byte
-- answer is replayed as a setter command, so the default set keeps it out of
-- JF's command range (1..14, 22). 'all' covers every high byte.
J = {}
-- crow truncates volts * 1638.3, so send from the middle of the step
local function jv(raw) return raw / 1638.3 end
local function jsend(raw) return (raw + 0.5) / 1638.3 end
local JRAW = {0x1800, 0x1b11, 0x1e22, 0x2133, 0x2444, 0x2755, 0x2a66, 0x2d77}
local JALL = {}
for i = 0, 63 do JALL[#JALL+1] = (i * 0x101 + 0x33) % 0x4000 end
ii.jf.event = function(e, v)
    if e.name == J.g1 then
        if v == J.g1want then J.ok = J.ok + 1 else J.bad = J.bad + 1; J.last_bad = e.name .. tostring(v)
            J.bads = (J.bads or '') .. ',' .. e.name .. tostring(v) end
        return
    elseif e.name ~= J.g2 then
        J.other = J.other + 1
        return
    end
    local want = J.fifo and table.remove(J.fifo, 1) or J.want
    -- JF applies setters on its next audio block (every 0.67 ms), so a query
    -- straight after a set may still see an earlier one: two earlier when two
    -- set+get pairs land inside one block (the metro jitters)
    local prev, prev2 = J.prev, J.prev2
    if J.fifo then J.prev2 = prev; J.prev = want end
    local function near(x) return x and math.abs(v - x) < 0.0003 end
    if near(want) then J.ok = J.ok + 1
    elseif near(prev) or near(prev2) then J.ok = J.ok + 1; J.late = J.late + 1
    else J.bad = J.bad + 1; J.last_bad = v; J.bads = (J.bads or '') .. ',' .. e.name .. tostring(v) end
end
-- crow A works on the speed switch and run; crow B on the mode switch and
-- transpose, so two leaders never send the same bytes (identical transfers
-- from two leaders merge into one on the wire, which hides arbitration)
function runj(n, period, per, kind, g1want, raw0, vals, who)
    local _ = ii.jf[1] -- JF at its first address (ii.jf[n] sticks)
    local V = (vals == 'all') and JALL or JRAW
    local g1, g2, set2, off = 'speed', 'run', ii.jf.run, 0
    if who == 'B' then g1, g2, set2, off = 'tsc', 'transpose', ii.jf.transpose, 1 end
    J = {sent=0, qsent=0, nsent=0, ok=0, bad=0, late=0, other=0, seq=0, g1=g1, g2=g2, g1want=g1want,
         want=jv(raw0), fifo=(kind == 'jset' or kind == 'jlong') and {} or nil}
    if J.fifo then J.prev = J.want; J.want = nil end
    -- swallow the answers to the bad getters before Lua looks them up
    ii.event_raw = function(addr, cmd) return addr == 0x70 and cmd ~= 143 and cmd ~= 131 and cmd ~= 144 and cmd ~= 132 end
    if MJ then metro.free(MJ.id) end
    MJ = metro.init{ time = period, count = n, event = function(c)
        for i = 1, per do
            local s = J.seq
            J.seq = s + 1
            if kind == 'jset' or kind == 'jlong' then
                local raw = V[(s + off * 3) % #V + 1]
                if kind == 'jset' then
                    set2(jsend(raw))
                else
                    ii.raw(0x70, string.char(who == 'B' and 4 or 3, raw >> 8, raw & 0xff, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa), 0)
                end
                J.sent = J.sent + 1
                table.insert(J.fifo, jv(raw))
                ii.jf.get(g2)
                J.qsent = J.qsent + 1
            elseif kind == 'jnote' or kind == 'jvox' then
                if kind == 'jnote' then ii.jf.play_note((s % 24 + off * 25) / 12, 0.5)
                else ii.jf.play_voice((s + off * 3) % 6 + 1, (s % 24) / 12, 0.5) end
                J.nsent = J.nsent + 1
                if s % 4 == 0 then ii.jf.get(g2); J.qsent = J.qsent + 1 end
            else -- 'jq', 'jbad'
                if kind == 'jbad' and s % 2 == 0 then
                    -- null getter, getters JF has commented out, past the end
                    local bad = {128, 133, 136, 137, 141, 150, 151, 200}
                    ii.raw(0x70, string.char(bad[(s // 2) % #bad + 1]), (s // 2) % 2 + 1)
                    J.sent = J.sent + 1
                end
                if s % 2 == 0 then ii.jf.get(g1) else ii.jf.get(g2) end
                J.qsent = J.qsent + 1
            end
        end
    end }
    MJ:start()
end
function reportj()
    local s = ii.stats()
    local out = {}
    for k, v in pairs(s) do out[#out+1] = k .. '=' .. v end
    for k, v in pairs(J) do if type(v) ~= 'table' then out[#out+1] = 'J.' .. k .. '=' .. tostring(v) end end
    print('REPORT ' .. table.concat(out, ' '))
end
