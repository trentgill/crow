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
