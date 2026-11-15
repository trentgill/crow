#!/usr/bin/env python3
"""Check the USB Audio function's descriptors (usbd/uac/usbd_uac.c) on the host.

Builds USBD_UAC_Descriptor() with the host's gcc (USBD_UAC_DESCRIPTOR_ONLY
leaves out everything that needs the HAL), dumps the bytes and checks them:
every bLength, the audio control header's wTotalLength, the terminal links,
the interface/alternate layout, and the endpoints against usbd/usbd_map.h.

    python3 tests/usb/uac_desc.py        # check, print a short summary
    python3 tests/usb/uac_desc.py -v     # and an annotated dump
"""

import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

# the constants the descriptor has to agree with, printed by the C side
CONSTANTS = [
    'ITF_AUDIO_AC', 'ITF_AUDIO_OUT', 'ITF_AUDIO_IN',
    'EP_AUDIO_OUT', 'EP_AUDIO_IN', 'EP_AUDIO_FB',
    'AUDIO_RATE_NOMINAL', 'AUDIO_OUT_CHANNELS', 'AUDIO_IN_CHANNELS',
    'AUDIO_BYTES', 'AUDIO_OUT_PACKET', 'AUDIO_IN_PACKET', 'AUDIO_FB_PACKET',
    'STR_IDX_AUDIO', 'STR_IDX_AUDIO_OUT', 'STR_IDX_AUDIO_IN',
    'USBD_UAC_DESC_SIZE',
]

DRIVER = r'''
#include <stdio.h>
#include "usbd_map.h"
#include "usbd_uac.h"
int main( void ){
    static uint8_t buf[1024];
    uint16_t n = USBD_UAC_Descriptor( buf );
    printf( "{\"desc\":\"" );
    for( int i=0; i<n; i++ ){ printf( "%02x", buf[i] ); }
    printf( "\"" );
@CONSTANTS@
    printf( "}\n" );
    return 0;
}
'''


def build():
    consts = ''.join('    printf( ",\\"%s\\":%%d", (int)(%s) );\n' % (c, c)
                     for c in CONSTANTS)
    with tempfile.TemporaryDirectory() as tmp:
        drv = os.path.join(tmp, 'drv.c')
        exe = os.path.join(tmp, 'drv')
        with open(drv, 'w') as f:
            f.write(DRIVER.replace('@CONSTANTS@', consts))
        subprocess.check_call([
            'gcc', '-std=c99', '-Wall', '-Werror', '-DUSBD_UAC_DESCRIPTOR_ONLY',
            '-I' + os.path.join(ROOT, 'usbd'),
            '-I' + os.path.join(ROOT, 'usbd', 'uac'),
            drv, os.path.join(ROOT, 'usbd', 'uac', 'usbd_uac.c'),
            '-o', exe])
        out = json.loads(subprocess.check_output([exe]))
    return bytes.fromhex(out.pop('desc')), out


def w(b, i):
    return b[i] | (b[i + 1] << 8)


def t(b, i):
    return b[i] | (b[i + 1] << 8) | (b[i + 2] << 16)


NAMES = {(0x0B, None): 'IAD', (0x04, None): 'INTERFACE', (0x05, None): 'ENDPOINT',
         (0x24, 1): 'AC HEADER / AS GENERAL', (0x24, 2): 'INPUT TERMINAL / FORMAT TYPE',
         (0x24, 3): 'OUTPUT TERMINAL', (0x25, 1): 'CS ENDPOINT GENERAL'}


def split(b):
    """[(offset, bytes)] for each descriptor, checking every bLength"""
    out, i = [], 0
    while i < len(b):
        n = b[i]
        assert n >= 2, 'bLength %d at offset %d' % (n, i)
        assert i + n <= len(b), 'descriptor at %d runs off the end' % i
        out.append((i, b[i:i + n]))
        i += n
    assert i == len(b)
    return out


def check(desc, c):
    fails = []

    def expect(cond, msg):
        if not cond:
            fails.append(msg)

    expect(len(desc) == c['USBD_UAC_DESC_SIZE'],
           'length %d != USBD_UAC_DESC_SIZE %d' % (len(desc), c['USBD_UAC_DESC_SIZE']))
    ds = split(desc)

    # IAD first, covering exactly the three audio interfaces
    iad = ds[0][1]
    expect(iad[1] == 0x0B and iad[0] == 8, 'first descriptor is not an 8 byte IAD')
    expect(iad[2] == c['ITF_AUDIO_AC'], 'IAD bFirstInterface')
    expect(iad[3] == 3, 'IAD bInterfaceCount')
    expect(iad[4] == 0x01, 'IAD bFunctionClass is not audio')
    expect(iad[7] == c['STR_IDX_AUDIO'], 'IAD iFunction')
    itf_numbers = {d[2] for _, d in ds if d[1] == 0x04}
    expect(itf_numbers == set(range(iad[2], iad[2] + iad[3])),
           'interfaces %s are not the IAD range' % sorted(itf_numbers))

    # group by interface descriptor: [(itf descriptor, [descriptors after it])]
    groups = []
    for _, d in ds[1:]:
        if d[1] == 0x04:
            groups.append((d, []))
        else:
            expect(groups, 'descriptor before the first interface')
            groups[-1][1].append(d)

    # audio control
    ac, ac_body = groups[0]
    expect(ac[2] == c['ITF_AUDIO_AC'] and ac[3] == 0, 'AC interface number/alt')
    expect(ac[4] == 0 and ac[5] == 1 and ac[6] == 1, 'AC: class/subclass/endpoints')
    hdr = ac_body[0]
    expect(hdr[1] == 0x24 and hdr[2] == 0x01, 'AC header missing')
    expect(w(hdr, 3) == 0x0100, 'bcdADC is not 1.0')
    total = sum(len(d) for d in ac_body)
    expect(w(hdr, 5) == total, 'AC wTotalLength %d != %d' % (w(hdr, 5), total))
    n_coll = hdr[7]
    coll = list(hdr[8:8 + n_coll])
    expect(len(hdr) == 8 + n_coll, 'AC header bLength vs bInCollection')
    expect(coll == [c['ITF_AUDIO_OUT'], c['ITF_AUDIO_IN']], 'baInterfaceNr %s' % coll)

    terms = {}
    for d in ac_body[1:]:
        expect(d[1] == 0x24, 'non class descriptor in AC body')
        if d[2] == 0x02:
            expect(len(d) == 12, 'input terminal bLength')
            terms[d[3]] = dict(kind='IT', type=w(d, 4), ch=d[7], cfg=w(d, 8), src=None)
        elif d[2] == 0x03:
            expect(len(d) == 9, 'output terminal bLength')
            terms[d[3]] = dict(kind='OT', type=w(d, 4), src=d[7])
        else:
            fails.append('unknown AC descriptor subtype %d' % d[2])
    expect(len(terms) == 4, 'expected 4 terminals, found %d' % len(terms))
    for tid, term in terms.items():
        expect(tid != 0, 'terminal id 0')
        if term['kind'] == 'OT':
            src = terms.get(term['src'])
            expect(src is not None and src['kind'] == 'IT',
                   'OT %d source %s is not an input terminal' % (tid, term['src']))
            if src:
                term['ch'] = src['ch']
                usb = [x for x in (term, src) if x['type'] == 0x0101]
                expect(len(usb) == 1, 'path into OT %d needs exactly one USB streaming end' % tid)
        # set channel counts on the input side

    # streaming interfaces: alt 0 empty, alt 1 streaming
    streams = {}
    for itf, body in groups[1:]:
        expect(itf[5] == 1 and itf[6] == 2, 'interface %d is not audio streaming' % itf[2])
        streams.setdefault(itf[2], {})[itf[3]] = (itf, body)
    expect(set(streams) == {c['ITF_AUDIO_OUT'], c['ITF_AUDIO_IN']}, 'streaming interfaces')

    def check_stream(num, direction, channels, ep_addr, packet, fb):
        alts = streams.get(num, {})
        expect(set(alts) == {0, 1}, 'interface %d alts %s' % (num, sorted(alts)))
        if set(alts) != {0, 1}:
            return
        a0, b0 = alts[0]
        expect(a0[4] == 0 and not b0, 'interface %d alt 0 must have no bandwidth' % num)
        a1, b1 = alts[1]
        eps = [d for d in b1 if d[1] == 0x05]
        expect(a1[4] == len(eps), 'interface %d bNumEndpoints %d, found %d' % (num, a1[4], len(eps)))
        expect(a1[8] == a0[8], 'interface %d alts name different strings' % num)
        gen = [d for d in b1 if d[1] == 0x24 and d[2] == 0x01]
        fmt = [d for d in b1 if d[1] == 0x24 and d[2] == 0x02]
        expect(len(gen) == 1 and len(gen[0]) == 7, 'AS general')
        expect(len(fmt) == 1, 'format type')
        if gen:
            link = terms.get(gen[0][3])
            expect(link is not None and link['type'] == 0x0101,
                   'interface %d bTerminalLink %d is not a USB streaming terminal' % (num, gen[0][3]))
            want = 'IT' if direction == 'out' else 'OT'
            expect(link is not None and link['kind'] == want,
                   'interface %d links to an %s, wants an %s' % (num, link and link['kind'], want))
            expect(w(gen[0], 5) == 1, 'wFormatTag is not PCM')
            if link:
                expect(link.get('ch') == channels,
                       'interface %d terminal has %s channels' % (num, link.get('ch')))
        if fmt:
            f = fmt[0]
            expect(len(f) == 8 + 3 * f[7], 'format bLength vs bSamFreqType')
            expect(f[3] == 1, 'format is not type I')
            expect(f[4] == channels, 'format bNrChannels %d != %d' % (f[4], channels))
            expect(f[5] == c['AUDIO_BYTES'] and f[6] == 8 * c['AUDIO_BYTES'], 'subframe/resolution')
            expect(f[7] == 1 and t(f, 8) == c['AUDIO_RATE_NOMINAL'], 'sample rate')
        # data endpoint first, its class-specific descriptor right after it
        expect(len(eps) >= 1, 'no data endpoint')
        if not eps:
            return
        data = eps[0]
        i = b1.index(data)
        expect(len(data) == 9, 'UAC1 endpoints are 9 bytes')
        expect(data[2] == ep_addr, 'interface %d data endpoint 0x%02x' % (num, data[2]))
        expect(bool(data[2] & 0x80) == (direction == 'in'), 'data endpoint direction')
        expect(data[3] == 0x05, 'data endpoint is not iso async (0x%02x)' % data[3])
        expect(w(data, 4) == packet, 'data wMaxPacketSize %d != %d' % (w(data, 4), packet))
        expect(packet == (c['AUDIO_RATE_NOMINAL'] // 1000 + 1) * channels * c['AUDIO_BYTES'],
               'packet size does not hold rate/1000 + 1 frames')
        expect(data[6] == 1, 'data bInterval')
        expect(data[7] == 0, 'data bRefresh')
        cs = b1[i + 1] if i + 1 < len(b1) else b''
        expect(len(cs) == 7 and cs[1] == 0x25 and cs[2] == 0x01,
               'class endpoint descriptor must follow the data endpoint')
        if cs:
            expect(cs[3] & 0x01, 'sampling frequency control bit')
        if fb is None:
            expect(len(eps) == 1, 'unexpected extra endpoint')
            expect(data[8] == 0, 'IN data endpoint bSynchAddress')
            return
        # explicit feedback. Linux: the second endpoint, iso, no bSynchAddress
        # of its own, and the data endpoint's bSynchAddress pointing at it
        expect(len(eps) == 2, 'feedback endpoint missing')
        if len(eps) < 2:
            return
        f = eps[1]
        expect(len(f) == 9, 'feedback endpoint is 9 bytes')
        expect(f[2] == fb and f[2] & 0x80, 'feedback endpoint address 0x%02x' % f[2])
        expect(data[8] == f[2], 'data bSynchAddress 0x%02x != feedback 0x%02x' % (data[8], f[2]))
        expect(f[3] & 0x03 == 0x01, 'feedback is not isochronous')
        expect(f[3] & 0x30 == 0x10, 'feedback usage type (0x%02x)' % f[3])
        expect(w(f, 4) == c['AUDIO_FB_PACKET'] == 3, 'feedback is 3 bytes (10.14) at full speed')
        expect(f[6] == 1, 'feedback bInterval')
        expect(1 <= f[7] <= 9, 'feedback bRefresh %d outside 1..9' % f[7])
        expect(f[8] == 0, 'feedback bSynchAddress')

    check_stream(c['ITF_AUDIO_OUT'], 'out', c['AUDIO_OUT_CHANNELS'],
                 c['EP_AUDIO_OUT'], c['AUDIO_OUT_PACKET'], c['EP_AUDIO_FB'])
    check_stream(c['ITF_AUDIO_IN'], 'in', c['AUDIO_IN_CHANNELS'],
                 c['EP_AUDIO_IN'], c['AUDIO_IN_PACKET'], None)

    # strings
    names = {streams[c['ITF_AUDIO_OUT']][1][0][8], streams[c['ITF_AUDIO_IN']][1][0][8]}
    expect(names == {c['STR_IDX_AUDIO_OUT'], c['STR_IDX_AUDIO_IN']}, 'streaming iInterface')
    expect(ac[8] == c['STR_IDX_AUDIO'], 'AC iInterface')
    return ds, terms, fails


def feedback_notes(c):
    nom = c['AUDIO_RATE_NOMINAL'] // 1000
    dac = 47.992
    fb = round(dac * (1 << 14))
    # Linux snd-usb-audio: freqn is 16.16 per frame; the first feedback value
    # is shifted until it lands in [freqn - freqn/4, freqn + freqn/2]
    freqn = nom << 16
    f, shift = fb, 0
    while f < freqn - freqn // 4:
        f <<= 1
        shift += 1
    while f > freqn + freqn // 2:
        f >>= 1
        shift -= 1
    return ('feedback at %.3fkHz: 0x%06x (10.14). Linux detects shift %d -> %.4f samples/frame'
            % (dac, fb, shift, f / 65536.0))


def main():
    desc, c = build()
    ds, terms, fails = check(desc, c)
    if '-v' in sys.argv:
        for off, d in ds:
            sub = d[2] if d[1] in (0x24, 0x25) else None
            name = NAMES.get((d[1], sub), '?')
            print('%3d  %-30s %s' % (off, name, d.hex(' ')))
        print()
    print('%d bytes, %d descriptors, terminals %s' % (
        len(desc), len(ds), ', '.join('%d:%s 0x%04x' % (k, v['kind'], v['type'])
                                      for k, v in sorted(terms.items()))))
    print(feedback_notes(c))
    for f in fails:
        print('FAIL', f)
    print('ok' if not fails else '%d failures' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
