-- temp placing print redef here to complete bootstrap elimination
print = function(...)
    local args = {...}
    local arg_len = #args
    local printResult = args[1]
    if arg_len > 1 then
        for i=2,arg_len do
            printResult = string.format('%s\t%s', printResult, tostring(args[i]))
        end
    end
    print_serial(tostring(printResult))
end

--- usb
-- usb.audio(true) re-enumerates crow as serial + MIDI + a 4 out, 2 in soundcard.
-- usb.audio() reads the mode.
usb = { audio = usb_audio_mode }

--- midi (usb)
-- midi.receive = function(status, data1, data2) ... end
-- midi.send(status, [data1], [data2])
midi = { send = usb_midi_send }
function midi_handler(...) if midi.receive then midi.receive(...) end end
