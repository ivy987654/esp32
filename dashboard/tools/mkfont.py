import sys
from fontTools import subset
from fontTools.ttLib import TTCollection
chars = set(chr(c) for c in range(0x20, 0x7F))
for hi in range(0xB0, 0xF8):
    for lo in range(0xA1, 0xFF):
        try: chars.add(bytes((hi, lo)).decode("gb2312"))
        except Exception: pass
for hi in range(0xA1, 0xAA):
    for lo in range(0xA1, 0xFF):
        try: chars.add(bytes((hi, lo)).decode("gb2312"))
        except Exception: pass
chars |= set(chr(c) for c in range(0x3040, 0x3100))   # kana
chars |= set(chr(c) for c in range(0xFF00, 0xFF5F))   # fullwidth
chars |= set("°·—…“”‘’《》【】、。，！？：；（）♪♫←→↑↓⌘⌥⇧⌃⏎⌫⇥␣")
font = TTCollection(sys.argv[1]).fonts[int(sys.argv[3])]
opt = subset.Options(); opt.layout_features = []; opt.hinting = False; opt.name_IDs = ["*"]; opt.notdef_outline = True
s = subset.Subsetter(opt); s.populate(text="".join(chars)); s.subset(font)
font.save(sys.argv[2]); print(len(chars))
