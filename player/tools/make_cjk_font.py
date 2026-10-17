"""Builds assets/fonts/NotoSansCJK-Subset.otf from a full Noto Sans CJK (SIL OFL 1.1).

Spotify falls back to Noto Sans for Japanese, Korean and Chinese, so the player does too.
The subset keeps: kana, all Hangul, CJK punctuation and full-width forms, general symbols
(music notes, arrows, shapes, stars), and the ideographs of JIS X 0208, GB 2312, Big5 and
KS X 1001 hanja, about 4 MB instead of 16 MB.

    py -3 make_cjk_font.py NotoSansCJKsc-Regular.otf out.otf
"""
import sys
from fontTools import subset
from fontTools.ttLib import TTFont

def decoded(codec, lead_range, trail_range):
    chars = set()
    for a in lead_range:
        for b in trail_range:
            try:
                text = bytes([a, b]).decode(codec)
            except (UnicodeDecodeError, ValueError):
                continue
            if len(text) == 1:
                chars.add(ord(text))
    return chars

keep = set()
for lo, hi in [(0x2000, 0x206F), (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2300, 0x23FF), (0x2460, 0x24FF),
               (0x25A0, 0x25FF), (0x2600, 0x26FF), (0x2700, 0x27BF), (0x3000, 0x303F), (0x3040, 0x30FF),
               (0x3130, 0x318F), (0x31F0, 0x31FF), (0x3200, 0x32FF), (0xAC00, 0xD7A3), (0x1100, 0x11FF),
               (0xFF00, 0xFFEF), (0x00A0, 0x024F)]:
    keep.update(range(lo, hi + 1))
keep |= decoded('shift_jis', range(0x81, 0xEF), range(0x40, 0xFD))
keep |= decoded('gb2312', range(0xA1, 0xF8), range(0xA1, 0xFF))
keep |= decoded('big5', range(0xA4, 0xFA), range(0x40, 0xFF))
keep |= decoded('euc_kr', range(0xCA, 0xFE), range(0xA1, 0xFF))
font = TTFont(sys.argv[1], lazy=True)
have = set(font.getBestCmap())
keep &= have
options = subset.Options()
options.layout_features = ['kern', 'vert', 'vrt2']
options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
options.notdef_outline = True
sub = subset.Subsetter(options)
sub.populate(unicodes=sorted(keep))
sub.subset(font)
font.save(sys.argv[2])
print(len(keep), 'characters')
