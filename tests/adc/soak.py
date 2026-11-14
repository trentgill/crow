import adcbench as a, time, sys
mins = float(sys.argv[1]) if len(sys.argv) > 1 else 10
c = a.Crow(); c.flash('/home/trent/d/crow-adc/crow.bin')
print('boot', c.boot_lines, flush=True)
d = a.dbg(c); print('at boot: mode', c.eval("adc_debug('get','mode')"), 'frames', d['frames'], 'resync', d['f_resync'], 'spi', d['f_spi'], flush=True)
# load: audio-rate lfo out, input stream events over usb, a busy metro
c.lua("output[1].action=lfo(0.001,4,'sine'); output[1]()")
c.lua("input[2].mode('stream',0.005)")
c.lua("busy=metro.init(function() local x=0 for i=1,3000 do x=x+math.sin(i) end end, 0.002) busy:start()")
c.lua("adc_debug('reset')"); c.telemetry_reset()
t0 = time.time(); tot = {}
while time.time() - t0 < mins*60:
    time.sleep(20)
    c.events.clear()
    d = a.dbg(c)
    print('%4.0fs frames %9d resync %d drdy %d spi %d other_flags %d slips %d dist %d blk %d..%d' % (time.time()-t0, d['frames'], d['f_resync'], d['f_drdy'], d['f_spi'], d['f_adcin']+d['f_opc']+d['f_check']+d['f_wdt'], d['slips'], d['ring_dist'], d['blk_min'], d['blk_max']), flush=True)
t = c.telemetry()
print('telemetry', {k: t[k] for k in ('cpu_busy','dsp_avg_us','dsp_max_us','tx_dropped','event_full') if k in t}, flush=True)
c.lua("busy:stop() input[2].mode('none') output[1].action=to(0,0) output[1]()")
