import sys
from fontTools.ttLib import TTFont, newTable
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
f = TTFont(sys.argv[1]); order = f.getGlyphOrder(); gs = f.getGlyphSet()
glyf = newTable("glyf"); glyf.glyphOrder = order; glyf.glyphs = {}
for n in order:
    pen = TTGlyphPen(gs); gs[n].draw(Cu2QuPen(pen, 1.0, reverse_direction=True)); glyf[n] = pen.glyph()
f["glyf"] = glyf; f["loca"] = newTable("loca")
for t in ("CFF ", "VORG", "BASE", "GPOS", "GSUB", "vhea", "vmtx"):
    if t in f: del f[t]
f["maxp"] = newTable("maxp"); m = f["maxp"]; m.tableVersion = 0x00010000
for a in ("maxZones","maxTwilightPoints","maxStorage","maxFunctionDefs","maxInstructionDefs","maxStackElements","maxSizeOfInstructions","maxComponentElements","maxPoints","maxContours","maxCompositePoints","maxCompositeContours","maxComponentDepth"):
    setattr(m, a, 0)
m.maxZones = 1; m.numGlyphs = len(order)
f["head"].glyphDataFormat = 0
f["post"].formatType = 3.0
f.sfntVersion = "\x00\x01\x00\x00"
f.save(sys.argv[2])
