"""Layouts of the ATEM "set value" commands, read off the recorded wire.

Each command changes values in one state field (and sometimes a second one
that shares them). A command is:  [mask][key bytes][values...]; the field is
[key bytes][values...] at other offsets. Only the values whose mask bit is
set change. After the change the switcher sends the field to every client —
unless nothing changed (then it sends nothing).

    prop(bit, name, cmd_offset, field_offset, size, signed=False, rule=None)

rule: None (stored as sent), ('clamp', lo, hi), ('mod', n) (wraps, e.g. hue),
('allow', {values}) — other values are refused: nothing changes and there is
no answer — or ('ignore',): always refused (a setting the Mini doesn't have).
echoSame=False: setting the value it already has gets no answer (sources);
otherwise the switcher answers every accepted set, changed or not.
effects={value: {other prop: value}}: setting this also sets another one.
More rules, for values that depend on other state or on the Mini's number
formats:
  ('unitwrap', lo)   the whole part (value / 1000) is kept in 16 bits, so a
                     huge value wraps; then at least lo (None: no minimum)
  ('negdec',)        a negative whole number is stored one lower (-1000 ->
                     -1001; recorded for -1, -9, -16, not for -27.536)
  ('allowbits', off) one bit, and one the field's byte at off allows
  ('eqfreq', off)    clamp to the range named by the field's byte at off
  ('refuseif', off, {values})  refused while the field's byte at off is one
                     of these
A command's echoSame=False applies to all its values. post names extra work
done after the values are stored (see check_setters.py / the emulator):
keyframeStored (KeFS), chromaCursor, transitionRate (a new rate of the
next transition's style is also its frames remaining in TrPs).

check_setters.py replays every recorded command through this table and
checks the switcher's recorded answers; gen_setters.py writes the C++ table
the emulator core uses.
"""


def prop(bit, name, cmd, field, size, signed=False, rule=None, also=None, echoSame=True, effects=None):
    return dict(bit=bit, name=name, cmd=cmd, field=field, size=size, signed=signed, rule=rule, also=also,
                echoSame=echoSame, effects=effects)


# Source ids (BMDSwitcherInputId) as recorded on the ATEM Mini.
BLACK, CAMS, BARS, COLOURS, MEDIA = {0}, {1, 2, 3, 4}, {1000}, {2001, 2002}, {3010, 3011}
PROGRAM, PREVIEW, CAM1_DIRECT = 10010, 10011, 11001


SETTERS = {
    # Luma key: KeLm = me, key, preMultiplied, _, clip, gain, inverse
    'CKLm': dict(field='KeLm', mask=1, key=[(1, 0), (2, 1)], props=[
        prop(0, 'preMultiplied', 3, 2, 1),
        prop(1, 'clip', 4, 4, 2),
        prop(2, 'gain', 6, 6, 2),
        prop(3, 'inverse', 8, 8, 1),
    ]),
    # Colour generator: ColV = index, _, hue, saturation, luma
    'CClV': dict(field='ColV', mask=1, key=[(1, 0)], props=[
        prop(0, 'hue', 2, 2, 2, rule=('mod', 3600)),
        prop(1, 'saturation', 4, 4, 2),
        prop(2, 'luma', 6, 6, 2),
    ]),
    # Aux (HDMI out) source: AuxS = aux, _, source
    'CAuS': dict(field='AuxS', mask=1, key=[(1, 0)], props=[
        prop(0, 'source', 2, 2, 2, echoSame=False, rule=('allow', CAMS | {PROGRAM, PREVIEW, CAM1_DIRECT})),
    ]),
    # Pattern key: KePt = me, key, pattern, _, size, symmetry, softness, hOffset, vOffset, inverse
    'CKPt': dict(field='KePt', mask=1, key=[(1, 0), (2, 1)], props=[
        # Choosing a pattern sets its symmetry (recorded per pattern).
        prop(0, 'pattern', 3, 2, 1, effects={'symmetry': {
            p: (10000 if p in (2, 3, 12, 13, 14, 15) else 8160 if p == 7 else 5000) for p in range(18)}}),
        prop(1, 'size', 4, 4, 2),
        prop(2, 'symmetry', 6, 6, 2),
        prop(3, 'softness', 8, 8, 2),
        prop(4, 'horizontalOffset', 10, 10, 2),
        prop(5, 'verticalOffset', 12, 12, 2),
        prop(6, 'inverse', 14, 14, 1),
    ]),
    # Dip transition: TDpP = me, rate, input
    'CTDp': dict(post='transitionRate', field='TDpP', mask=1, key=[(1, 0)], props=[
        prop(0, 'rate', 2, 1, 1, rule=('clamp', 1, 250)),
        prop(1, 'input', 4, 2, 2, echoSame=False, rule=('allow', BLACK | CAMS | BARS | COLOURS | MEDIA)),
    ]),
    # DVE transition: TDvP = me, rate, logoRate, style, fill, key, enableKey,
    # preMultiplied, clip, gain, inverse, reverse, flipFlop. The key settings
    # are shared with the stinger transition (TStP).
    'CTDv': dict(post='transitionRate', field='TDvP', mask=2, key=[(2, 0)], props=[
        prop(0, 'rate', 3, 1, 1, rule=('clamp', 1, 250)),
        prop(1, 'logoRate', 4, 2, 1, rule=('clamp', 1, 250)),
        prop(2, 'style', 5, 3, 1, rule=('allow', set(range(16, 32)) | {34})),
        prop(3, 'inputFill', 6, 4, 2, echoSame=False, rule=('allow', BLACK | CAMS | BARS | COLOURS | MEDIA)),
        prop(4, 'inputCut', 8, 6, 2, echoSame=False, rule=('allow', BLACK | CAMS | BARS | MEDIA)),
        prop(5, 'enableKey', 10, 8, 1),
        prop(6, 'preMultiplied', 11, 9, 1, also=('TStP', 3)),
        prop(7, 'clip', 12, 10, 2, also=('TStP', 4)),
        prop(8, 'gain', 14, 12, 2, also=('TStP', 6)),
        prop(9, 'inverse', 16, 14, 1, also=('TStP', 8)),
        prop(10, 'reverse', 17, 15, 1),
        prop(11, 'flipFlop', 18, 16, 1),
    ]),

    # Advanced chroma key: KACk = me, key, foreground, background, keyEdge,
    # spill, flare, brightness, contrast, saturation, red, green, blue
    'CACK': dict(echoSame=False, field='KACk', mask=2, key=[(2, 0), (3, 1)], props=[
        prop(0, 'foregroundLevel', 4, 2, 2),
        prop(1, 'backgroundLevel', 6, 4, 2),
        prop(2, 'keyEdge', 8, 6, 2),
        prop(3, 'spillSuppress', 10, 8, 2),
        prop(4, 'flareSuppress', 12, 10, 2),
        prop(5, 'brightness', 14, 12, 2, signed=True),
        prop(6, 'contrast', 16, 14, 2, signed=True),
        prop(7, 'saturation', 18, 16, 2),
        prop(8, 'red', 20, 18, 2, signed=True),
        prop(9, 'green', 22, 20, 2, signed=True),
        prop(10, 'blue', 24, 22, 2, signed=True),
    ]),
    # Advanced chroma sample: KACC = me, key, cursor, preview, cursorX, cursorY,
    # cursorSize, y, cb, cr. Cursor and preview on/off: the Mini refuses them.
    'CACC': dict(echoSame=False, field='KACC', mask=1, key=[(1, 0), (2, 1)], post='chromaCursor', props=[
        prop(0, 'samplingModeEnabled', 3, 2, 1, rule=('ignore',)),
        prop(1, 'previewEnabled', 4, 3, 1, rule=('ignore',)),
        prop(2, 'cursorXPosition', 6, 4, 2, signed=True),
        prop(3, 'cursorYPosition', 8, 6, 2, signed=True),
        prop(4, 'cursorSize', 10, 8, 2, signed=True, rule=('clamp', 620, 9925)),
        prop(5, 'sampledY', 12, 10, 2, signed=True, rule=('clamp', 0, 10000)),
        prop(6, 'sampledCb', 14, 12, 2, signed=True, rule=('clamp', 0, 10000)),
        prop(7, 'sampledCr', 16, 14, 2, signed=True, rule=('clamp', 0, 10000)),
    ]),
    # Fly keyframe A/B values: KKFP = me, key, keyframe, _, sizeX, sizeY,
    # positionX, positionY, rotation, borderWidthOut, borderWidthIn,
    # softnessOut, softnessIn, bevelSoftness, bevelPosition, opacity, _,
    # hue, saturation, luma, lightDirection, lightAltitude, _, mask T/B/L/R.
    # Setting any value marks the keyframe as stored (KeFS): post action.
    'CKFP': dict(field='KKFP', mask=4, key=[(4, 0), (5, 1), (6, 2)], post='keyframeStored', props=[
        prop(0, 'sizeX', 8, 4, 4, signed=True, rule=('unitwrap', 0)),
        prop(1, 'sizeY', 12, 8, 4, signed=True, rule=('unitwrap', 0)),
        prop(2, 'positionX', 16, 12, 4, signed=True, rule=('unitwrap', None)),
        prop(3, 'positionY', 20, 16, 4, signed=True, rule=('unitwrap', None)),
        prop(4, 'rotation', 24, 20, 4, signed=True, rule=('ignore',)),
        prop(5, 'borderWidthOut', 28, 24, 2),
        prop(6, 'borderWidthIn', 30, 26, 2),
        prop(7, 'borderSoftnessOut', 32, 28, 1),
        prop(8, 'borderSoftnessIn', 33, 29, 1),
        prop(9, 'borderBevelSoftness', 34, 30, 1, rule=('ignore',)),
        prop(10, 'borderBevelPosition', 35, 31, 1, rule=('ignore',)),
        prop(11, 'borderOpacity', 36, 32, 1),
        prop(12, 'borderHue', 38, 34, 2),   # no wrap here, unlike CKDV
        prop(13, 'borderSaturation', 40, 36, 2),
        prop(14, 'borderLuma', 42, 38, 2),
        prop(15, 'borderLightSourceDirection', 44, 40, 2, rule=('mod', 3600)),
        prop(16, 'borderLightSourceAltitude', 46, 42, 1),
        prop(17, 'maskTop', 48, 44, 2, signed=True, rule=('negdec',)),
        prop(18, 'maskBottom', 50, 46, 2, signed=True, rule=('negdec',)),
        prop(19, 'maskLeft', 52, 48, 2, signed=True, rule=('negdec',)),
        prop(20, 'maskRight', 54, 50, 2, signed=True, rule=('negdec',)),
    ]),
    # DVE key (the PiP): KeDV = me, key, _, sizeX, sizeY, positionX, positionY,
    # rotation, borderEnabled, shadow, bevel, borderWidthOut/In, softnessOut/In,
    # bevelSoftness/Position, opacity, _, hue, saturation, luma, lightDirection,
    # lightAltitude, masked, mask T/B/L/R, rate
    'CKDV': dict(field='KeDV', mask=4, key=[(4, 0), (5, 1)], props=[
        prop(0, 'sizeX', 8, 4, 4, signed=True, rule=('unitwrap', 0)),
        prop(1, 'sizeY', 12, 8, 4, signed=True, rule=('unitwrap', 0)),
        prop(2, 'positionX', 16, 12, 4, signed=True, rule=('unitwrap', None)),
        prop(3, 'positionY', 20, 16, 4, signed=True, rule=('unitwrap', None)),
        prop(4, 'rotation', 24, 20, 4, signed=True),
        prop(5, 'borderEnabled', 28, 24, 1),
        prop(6, 'shadow', 29, 25, 1),
        prop(7, 'borderBevel', 30, 26, 1, rule=('ignore',)),
        prop(8, 'borderWidthOut', 32, 28, 2),
        prop(9, 'borderWidthIn', 34, 30, 2),
        prop(10, 'borderSoftnessOut', 36, 32, 1),
        prop(11, 'borderSoftnessIn', 37, 33, 1),
        prop(12, 'borderBevelSoftness', 38, 34, 1, rule=('ignore',)),
        prop(13, 'borderBevelPosition', 39, 35, 1, rule=('ignore',)),
        prop(14, 'borderOpacity', 40, 36, 1),
        prop(15, 'borderHue', 42, 38, 2, rule=('mod', 3600)),
        prop(16, 'borderSaturation', 44, 40, 2),
        prop(17, 'borderLuma', 46, 42, 2),
        prop(18, 'borderLightSourceDirection', 48, 44, 2, rule=('mod', 3600)),
        prop(19, 'borderLightSourceAltitude', 50, 46, 1),
        prop(20, 'masked', 51, 47, 1),
        prop(21, 'maskTop', 52, 48, 2, signed=True, rule=('negdec',)),
        prop(22, 'maskBottom', 54, 50, 2, signed=True, rule=('negdec',)),
        prop(23, 'maskLeft', 56, 52, 2, signed=True, rule=('negdec',)),
        prop(24, 'maskRight', 58, 54, 2, signed=True, rule=('negdec',)),
        prop(25, 'rate', 60, 56, 1),
    ]),
    # Wipe transition: TWpP = me, rate, pattern, _, width, inputBorder,
    # symmetry, softness, x, y, reverse, flipFlop
    'CTWp': dict(post='transitionRate', field='TWpP', mask=2, key=[(2, 0)], props=[
        prop(0, 'rate', 3, 1, 1, echoSame=False, rule=('clamp', 1, 250)),
        prop(1, 'pattern', 4, 2, 1, effects={'symmetry': {
            p: (10000 if p in (2, 3, 12, 13, 14, 15) else 8160 if p == 7 else 5000) for p in range(18)}}),
        prop(2, 'borderSize', 6, 4, 2),
        prop(3, 'inputBorder', 8, 6, 2, echoSame=False, rule=('allow', BLACK | CAMS | BARS | COLOURS | MEDIA)),
        prop(4, 'symmetry', 10, 8, 2, rule=('ignore',)),   # refused (recorded with pattern 6 only)
        prop(5, 'softness', 12, 10, 2),
        prop(6, 'horizontalOffset', 14, 12, 2),
        prop(7, 'verticalOffset', 16, 14, 2),
        prop(8, 'reverse', 18, 16, 1),
        prop(9, 'flipFlop', 19, 17, 1),
    ]),
    # Mix transition: TMxP = me, rate (no mask)
    'CTMx': dict(post='transitionRate', field='TMxP', mask=0, key=[(0, 0)], props=[
        prop(None, 'rate', 1, 1, 1, rule=('clamp', 1, 250)),
    ]),
    # Time code mode: TCCc = mode (no mask)
    'CTCC': dict(field='TCCc', mask=0, key=[], props=[
        prop(None, 'mode', 0, 0, 1),
    ]),
    # Audio follow video crossfade: FMPP = followVideo
    'CMPP': dict(echoSame=False, field='FMPP', mask=1, key=[], props=[
        prop(0, 'audioFollowVideoCrossfadeTransition', 1, 0, 1),
    ]),
}

# Fairlight commands share a key: input id (2 bytes at 2 -> 0) and source id
# (8 bytes at 8 -> 8); values follow at the same offsets in command and field.
FL_KEY = [(2, 0), (3, 1)] + [(i, i) for i in range(8, 16)]

SETTERS.update({
    # Fairlight source: FASP = input, _, source, type, maxDelay, delay, _,
    # inputGain, _, stereoSim, bands, eqEnabled, _, eqGain, makeupGain, pan,
    # _, faderGain, supportedMixOptions, mixOption
    'CFSP': dict(echoSame=False, field='FASP', mask=2, key=FL_KEY, props=[
        prop(0, 'delayFrames', 16, 18, 1, rule=('ignore',)),
        prop(1, 'inputGain', 20, 20, 4, signed=True, rule=('clamp', -10000, 600)),
        prop(2, 'stereoSimulationIntensity', 24, 26, 2, rule=('ignore',)),
        prop(3, 'eqEnabled', 26, 29, 1),
        prop(4, 'eqGain', 28, 32, 4, signed=True, rule=('clamp', -2000, 2000)),
        prop(5, 'makeupGain', 32, 36, 4, signed=True, rule=('clamp', 0, 2000)),
        prop(6, 'pan', 36, 40, 2, signed=True, rule=('clamp', -10000, 10000)),
        prop(7, 'faderGain', 40, 44, 4, signed=True, rule=('clamp', -10000, 1000)),
        prop(8, 'mixOption', 44, 49, 1),
    ]),
    # Compressor: AICP = key, enabled, _, threshold, ratio, _, attack, hold, release
    'CICP': dict(echoSame=False, field='AICP', mask=1, key=FL_KEY, props=[
        prop(0, 'enabled', 16, 16, 1),
        prop(1, 'threshold', 20, 20, 4, signed=True, rule=('clamp', -5000, 0)),
        prop(2, 'ratio', 24, 24, 2, rule=('clamp', 120, 2000)),
        prop(3, 'attack', 28, 28, 4, signed=True, rule=('clamp', 70, 10000)),
        prop(4, 'hold', 32, 32, 4, signed=True, rule=('clamp', 0, 400000)),
        prop(5, 'release', 36, 36, 4, signed=True, rule=('clamp', 5000, 400000)),
    ]),
    # Limiter: AILP = key, enabled, _, threshold, attack, hold, release
    'CILP': dict(echoSame=False, field='AILP', mask=1, key=FL_KEY, props=[
        prop(0, 'enabled', 16, 16, 1),
        prop(1, 'threshold', 20, 20, 4, signed=True, rule=('clamp', -3000, 0)),
        prop(2, 'attack', 24, 24, 4, signed=True, rule=('clamp', 70, 3000)),
        prop(3, 'hold', 28, 28, 4, signed=True, rule=('clamp', 0, 400000)),
        prop(4, 'release', 32, 32, 4, signed=True, rule=('clamp', 5000, 400000)),
    ]),
    # Expander: AIXP = key, enabled, gateMode, _, threshold, range, ratio,
    # attack, hold, release
    'CIXP': dict(echoSame=False, field='AIXP', mask=1, key=FL_KEY, props=[
        prop(0, 'enabled', 16, 16, 1),
        prop(1, 'gateMode', 17, 17, 1),
        prop(2, 'threshold', 20, 20, 4, signed=True, rule=('clamp', -5000, 0)),
        prop(3, 'range', 24, 24, 2, rule=('clamp', 0, 6000)),
        prop(4, 'ratio', 26, 26, 2, rule=('clamp', 110, 300)),
        prop(5, 'attack', 28, 28, 4, signed=True, rule=('clamp', 50, 10000)),
        prop(6, 'hold', 32, 32, 4, signed=True, rule=('clamp', 0, 400000)),
        prop(7, 'release', 36, 36, 4, signed=True, rule=('clamp', 5000, 400000)),
    ]),
    # EQ band: AEBP = key, band, enabled, supportedShapes, shape,
    # supportedRanges, range, _, frequency, gain, qFactor
    'CEBP': dict(echoSame=False, field='AEBP', mask=1, key=FL_KEY + [(16, 16)], props=[
        prop(0, 'enabled', 17, 17, 1),
        prop(1, 'shape', 18, 19, 1, rule=('allowbits', 18)),
        prop(2, 'frequencyRange', 19, 21, 1, rule=('allowbits', 20)),
        prop(3, 'frequency', 20, 24, 4, rule=('eqfreq', 21)),
        prop(4, 'gain', 24, 28, 4, signed=True, rule=('clamp', -2000, 2000)),
        prop(5, 'qFactor', 28, 32, 2, rule=('refuseif', 19, {2, 16})),
    ]),
    # Analog audio input (the mics): FAIP = input, ..., supportedLevels at 11,
    # level at 12
    'CFEP': dict(echoSame=False, field='FAIP', mask=1, key=[(2, 0), (3, 1)], props=[
        prop(1, 'inputLevel', 5, 12, 1, rule=('allowbits', 11)),
    ]),
    # Audio input configuration: FAIP supportedConfigurations at 9,
    # configuration at 10 (mono, stereo, dual mono). Dual mono also splits the
    # source in two on the real switcher (new FASP etc.); not emulated.
    'CFIP': dict(echoSame=False, field='FAIP', mask=1, key=[(2, 0), (3, 1)], props=[
        prop(0, 'configuration', 4, 10, 1, rule=('allowbits', 9)),
    ]),
    # Fairlight master: FAMP = bands, eqEnabled, eqGain, makeupGain, ...,
    # faderGain at 12, followFadeToBlack at 16
    'CFMP': dict(echoSame=False, field='FAMP', mask=1, key=[], props=[
        prop(3, 'masterOutFaderGain', 12, 12, 4, signed=True, rule=('clamp', -10000, 1000)),
        prop(4, 'masterOutFollowFadeToBlack', 16, 16, 1),
    ]),
})

# Fairlight EQ band frequency ranges (GetFrequencyRangeMinMax on the Mini).
EQ_RANGES = {1: (30, 395), 2: (100, 1480), 4: (450, 7910), 8: (1400, 21700)}

# Commands that aren't setters but send setter fields (reset EQ, reset
# dynamics, store keyframe, ...). The checker skips tests that use them; the
# emulator's verify run checks them.
ACTIONS = {'SFKF', 'RFKF', 'RICE', 'RICD', 'RICC', 'RFIP', 'RFLP'}
