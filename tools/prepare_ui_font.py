"""Freeze Noto Sans SC variable font at readable medium weight."""
import sys
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
font = TTFont(sys.argv[1])
if "fvar" in font:
    font = instantiateVariableFont(font, {"wght": 500}, inplace=True)
font.save(sys.argv[2])
