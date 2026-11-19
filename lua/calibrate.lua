--- cv i/o calibration interface

-- o = cal.input[1].offset -- read the offset value (default 0.0)
-- cal.input[1].offset = o + 0.001 -- update the offset value

-- o = cal.input[1].scale -- read the scale value (default 1.0)
-- cal.input[1].scale = o * 1.001 -- update the scale value

-- cal.source(1) -- set input[1] source to output 1
-- cal.source(2) -- set input[1] source to output 2
-- cal.source(3) -- set input[1] source to output 3
-- cal.source(4) -- set input[1] source to output 4
-- cal.source'gnd' -- set input[1] source to ground
-- cal.source'2v5' -- set input[1] source to 2.5V reference

-- cal.save() -- write updated scale & offset values

-- v1, v2 = cal.read() -- mean of both inputs over every ADC sample (inside a clock)
-- v1, v2 = cal.read(count, settle) -- average `count` samples after `settle` seconds
-- cal.average(count, skip) / cal.average() -- the same without waiting: nil until done

Cal = {
    input = {},
    output = {},
    mt = {
        __index = function(self, ix)
            return calibrate_get(self.chan, ix)
        end,
        __newindex = function(self, ix, val)
            return calibrate_set(self.chan, ix, val)
        end
    },
    save = calibrate_save,
    source = calibrate_source,
    average = calibrate_average,
    rate = 46875, -- ADC samples per second
}

-- must be called from inside a clock coroutine
function Cal.read(count, settle)
    calibrate_average(count or 2048, math.floor((settle or 0.005) * Cal.rate + 0.5))
    local a, b
    repeat
        clock.sleep(0.001)
        a, b = calibrate_average()
    until a
    return a, b
end

function Cal.new_cal(chan)
    return setmetatable({chan = chan}, Cal.mt)
end

Cal.input[1]  = Cal.new_cal(1)
Cal.input[2]  = Cal.new_cal(2)
Cal.output[1] = Cal.new_cal(3)
Cal.output[2] = Cal.new_cal(4)
Cal.output[3] = Cal.new_cal(5)
Cal.output[4] = Cal.new_cal(6)

return Cal
