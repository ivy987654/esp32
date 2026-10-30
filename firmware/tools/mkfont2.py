"""Subset a TTF to GB2312 + Big5 level-1 + kana + ASCII/punct for the board."""
import sys
from fontTools import subset
from fontTools.ttLib import TTFont
chars = set(chr(c) for c in range(0x20, 0x7F))
def add(enc, his, los):
    for hi in his:
        for lo in los:
            try: chars.add(bytes((hi, lo)).decode(enc))
            except Exception: pass
add("gb2312", range(0xA1, 0xF8), range(0xA1, 0xFF))
add("big5", range(0xA4, 0xC7), list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)))
chars |= set(chr(c) for c in range(0x3040, 0x3100))
chars |= set(chr(c) for c in range(0xFF00, 0xFF5F))
chars |= set("°·—…“”‘’《》【】、。，！？：；（）♪←→●Ⅱ「」~～")
f = TTFont(sys.argv[1])
opt = subset.Options(); opt.layout_features = []; opt.hinting = False; opt.notdef_outline = True
s = subset.Subsetter(opt); s.populate(text="".join(chars)); s.subset(f)
f.save(sys.argv[2]); print(len(chars))
