--- cali
-- utility for calibrating crow's CV i/o
--
-- keep druid open while running!
--
-- 1. remove every patch cable. calibration starts by itself:
--    input[1], all 4 outputs and input[2]'s offset are calibrated,
--    then input[1] and the outputs are tested.
-- 2. when asked, connect output[1] -> input[2].
--    input[2] is calibrated and tested. everything is saved if every test passed.
--    you can then remove the cable.
--
-- re-run the calibration with: calibrate()
-- run the test-suite with: test()
-- print the raw offsets & scalars with: raw()
--
-- method: every channel is linear, so two readings solve its scale and offset
-- exactly. there is nothing to converge: a second pass changes nothing beyond noise.
-- firmware with cal.average() averages every ADC sample (46.875kHz) in C,
-- so a reading takes ~50ms. older firmware falls back to polling input[n].volts.


-- settings
local AUTOSTART  = true  -- if false, you can calibrate by typing: calibrate()
local AUTOSAVE   = true  -- save to flash when every test passes. set false while debugging
local ALWAYS_RAW = false -- print raw offset & scale values after calibrate()

local SAMPLES = 2048  -- ADC samples per reading (43.7ms). 1024 matches the old 256-poll noise
local SETTLE  = 0.005 -- seconds between changing a source and averaging
local V_LO, V_HI = 0, 2.5 -- calibration points for the outputs and input[2]

local FAST = cal.average ~= nil


--- measurement

-- returns input[1], input[2] in volts. must be called inside a clock
local function read_both()
    if FAST then return cal.read(SAMPLES, SETTLE) end
    -- older firmware: one new sample per block, so poll and average
    local reps = 256
    local a, b = 0, 0
    clock.sleep(0.02) -- input & output settling time
    for i=1,reps do
        a = a + input[1].volts
        b = b + input[2].volts
        clock.sleep(0.001)
    end
    return a/reps, b/reps
end

local function read(n)
    local a, b = read_both()
    return n == 2 and b or a
end

-- set output[ch] to v and every other output to 0
local function xvolts(ch, v)
    for n=1,4 do
        output[n].volts = n==ch and v or 0
    end
end

-- input[1] through the cal mux, from output[n] at v
local function via_mux(n, v)
    xvolts(n, v)
    cal.source(n)
    return read(1)
end

-- input[2] through the patch cable, from output[1] at v
local function via_cable(v)
    xvolts(1, v)
    cal.source'gnd'
    return read(2)
end


--- exact two-point solutions
-- each reading m was taken with the calibration currently in place

-- adc: v = raw*k*scale + 2.5 + offset. readings m1, m2 of true volts x1, x2
local function solve_input(n, x1, m1, x2, m2)
    local c = cal.input[n]
    local s0, o0 = c.scale, c.offset
    local s = s0 * (x2 - x1) / (m2 - m1)
    local o = x1 - 2.5 - (m1 - 2.5 - o0) * s / s0
    c.scale, c.offset = s, o
end

-- adc offset only, from a reading m of 0V
local function solve_input_offset(n, m)
    cal.input[n].offset = cal.input[n].offset - m
end

-- dac: code = (v + offset)*k*scale. m1, m2 read back at requested volts v1, v2
local function solve_output(n, v1, m1, v2, m2)
    local c = cal.output[n]
    local s0, o0 = c.scale, c.offset
    local gain = (m2 - m1) / (v2 - v1) -- volts out per volt requested, now
    c.scale = s0 / gain
    c.offset = gain * (v1 + o0) - m1
end


--- jacks & cables

local function out_of(lo, hi, v) return v < lo or v > hi end

-- every jack whose cable breaks the internal paths. empty when none
local function cables_in()
    -- input[1]'s jack normals to the cal mux, each output's jack normals to the mux
    xvolts(0, 0)
    cal.source'2v5'
    if out_of(1.5, 3.5, read(1)) then return {'input[1]'} end
    local found = {}
    for n=1,4 do
        if out_of(-4, -2, via_mux(n, -3)) then table.insert(found, 'output['..n..']') end
    end
    xvolts(0, 0)
    return found
end

local function await_no_cables()
    local found = cables_in()
    if #found == 0 then return end
    print('\nremove the patch cable from: '..table.concat(found, ', '))
    while #found > 0 do
        clock.sleep(0.2)
        found = cables_in()
    end
    print'ok!'
    clock.sleep(1) -- let both ends of the cable come out
end

-- output[1] -> input[2]: input[2] follows output[1]
local function cable_out1_in2()
    return not out_of(-4, -2, via_cable(-3)) and not out_of(2, 4, via_cable(3))
end

local function await_out1_in2()
    if cable_out1_in2() then return end
    print '\nconnect output[1] -> input[2]'
    while not cable_out1_in2() do
        tell('stream', 2, input[2].volts)
        clock.sleep(0.1)
    end
    print'ok!'
    clock.sleep(0.5) -- let the cable seat
end


--- calibration steps

local function clear_cal_settings()
    for n=1,2 do
        cal.input[n].offset = 0
        cal.input[n].scale = 1.0
    end
    for n=1,4 do
        cal.output[n].offset = 0
        cal.output[n].scale = 1.0
    end
end

local function cal_input1()
    xvolts(0, 0)
    cal.source'gnd'
    local gnd, in2 = read_both() -- input[2]'s jack normals to ground: its offset comes free
    cal.source'2v5'
    local vref = read(1)
    solve_input(1, 0, gnd, 2.5, vref)
    solve_input_offset(2, in2)
    print(string.format('input[1]  scale % 7.5f  offset % 7.5f  (% 6.4f % 6.4f)'
        , cal.input[1].scale, cal.input[1].offset, gnd, vref))
end

local function cal_output(n)
    local lo = via_mux(n, V_LO)
    local hi = via_mux(n, V_HI)
    solve_output(n, V_LO, lo, V_HI, hi)
    print(string.format('output[%i] scale % 7.5f  offset % 7.5f  (% 6.4f % 6.4f)'
        , n, cal.output[n].scale, cal.output[n].offset, lo, hi))
end

local function cal_input2() -- needs output[1] -> input[2]
    local lo = via_cable(V_LO)
    local hi = via_cable(V_HI)
    solve_input(2, V_LO, lo, V_HI, hi)
    print(string.format('input[2]  scale % 7.5f  offset % 7.5f  (% 6.4f % 6.4f)'
        , cal.input[2].scale, cal.input[2].offset, lo, hi))
end

-- values no working unit produces (wrong cable, dead channel)
local function implausible()
    local bad = {}
    local function chk(name, c)
        if out_of(0.8, 1.2, c.scale) or out_of(-0.2, 0.2, c.offset) then
            table.insert(bad, name)
        end
    end
    for n=1,2 do chk('input['..n..']', cal.input[n]) end
    for n=1,4 do chk('output['..n..']', cal.output[n]) end
    return bad
end


--- tests

local function cents(result, expected)
    return math.floor(0.5 + 1200*(result - expected))
end

local OUT_V = {-2.5, 0, 2.5,  5, 7.5} -- test voltages
local OUT_E = {  30, 3,   5, 12,  30} -- allowable cents error at each voltage

local function new_result()
    local r = {pass = true, fails = {}}
    function r.check(name, got, want, tol)
        local e = cents(got, want)
        if math.abs(e) > tol then
            r.pass = false
            table.insert(r.fails, name..' @'..want..'V out of range')
        end
        return e
    end
    return r
end

-- input[1], input[2]'s ground and the outputs, with no cables
local function test_internal(r)
    print'\nvoltage listed to the nearest millivolt'
    print'error amounts listed in cents beneath the readings'
    print'  expect:   -2.5    0.0    2.5    5.0    7.5\n'

    xvolts(0, 0)
    cal.source'gnd'
    local gnd, gnd2 = read_both()
    cal.source'2v5'
    local vref = read(1)
    local e1 = r.check('input[1]', gnd, 0, 3)
    local e2 = r.check('input[1]', vref, 2.5, 3)
    print(string.format('input[1]           % 01.3f % 01.3f', gnd, vref))
    print(string.format('              % 7d % 7d', e1, e2))
    local e3 = r.check('input[2]', gnd2, 0, 3)
    print(string.format('input[2]           % 01.3f', gnd2))
    print(string.format('              % 7d', e3))

    for n=1,4 do
        local t, e = {}, {}
        for k,v in ipairs(OUT_V) do
            t[k] = via_mux(n, v)
            e[k] = r.check('output['..n..']', t[k], v, OUT_E[k])
        end
        print(string.format('output[%i]   % 01.3f % 01.3f % 01.3f % 01.3f % 01.3f',n
             ,t[1],t[2],t[3],t[4],t[5]))
        print(string.format('       % 7d % 7d % 7d % 7d % 7d',e[1],e[2],e[3],e[4],e[5]))
    end
    xvolts(0, 0)
end

-- input[2] against output[1] over the outputs' range. needs the cable
local function test_input2(r)
    local t, e = {}, {}
    for k,v in ipairs(OUT_V) do
        t[k] = via_cable(v)
        e[k] = r.check('input[2]', t[k], v, OUT_E[k])
    end
    print(string.format('input[2]    % 01.3f % 01.3f % 01.3f % 01.3f % 01.3f'
         ,t[1],t[2],t[3],t[4],t[5]))
    print(string.format('       % 7d % 7d % 7d % 7d % 7d',e[1],e[2],e[3],e[4],e[5]))
    xvolts(0, 0)
end

local function report(r)
    if r.pass then
        print('\n---- PASS ----')
    else
        print('\n!!!! FAIL !!!!')
        print(table.concat(r.fails, ", "))
    end
end

function test()
    clock.run(function()
        await_no_cables()
        local r = new_result()
        test_internal(r)
        report(r)
    end)
end

function raw()
    local function pprintset(key)
        print(key..'s: ')
        for k,v in ipairs(cal[key]) do
            print(string.format('  %i  offset: % 01.4f,  scale:% 01.4f', k, v.offset, v.scale))
        end
    end
    print' offset should be near 0'
    print' scale should be near 1'
    pprintset'input'
    pprintset'output'
end

local function elapsed(t0)
    print(string.format('(%.2fs)', (time() - t0) / 1000))
end

function calibrate()
    clock.run( function()
        if not FAST then print'(older firmware: each reading averages slowly)' end
        await_no_cables()
        local t0 = time()
        local r = new_result()

        clear_cal_settings() -- clear everything before we start
        cal_input1()
        for n=1,4 do cal_output(n) end
        test_internal(r)
        elapsed(t0)

        await_out1_in2()
        t0 = time()
        cal_input2()
        test_input2(r)
        elapsed(t0)

        local bad = implausible()
        if #bad > 0 then
            r.pass = false
            table.insert(r.fails, 'implausible calibration for '..table.concat(bad, ', '))
        end
        report(r)

        if ALWAYS_RAW then raw() end
        if AUTOSAVE then
            if r.pass then cal.save()
            else print'calibration NOT saved. fix the fault and run calibrate(), or keep it with cal.save()' end
        end
        print'you can remove the patch cable'
    end)
end

function init()
    print('///////////////////////////')
    if AUTOSTART then calibrate() end
end
