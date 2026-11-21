#!/usr/bin/env python3
"""Erzeugt skins/ (TUI.json, Q-Links.json, PNGs) fuer MPC Folder Sync.
Aufruf: python3 skin-src/make_skin.py   (im Projektordner)
Nutzt die Hilfsfunktionen von tools/gen_ui.py. Ergebnis wird von build.py unveraendert uebernommen."""
import os, sys, glob
HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(PROJ, "..", "..", "tools"))
import gen_ui as g
import make_ui_kit
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(PROJ, "skins")
os.makedirs(OUT, exist_ok=True)
S = dict(g.DEFAULT_STYLE)
STATUS = ["Ready", "Running", "Done", "Failed", "No rsync", "No ssh", "No config", "Bad target", "Cannot run"]

# --- Bilder -------------------------------------------------------------------------------
def font(size):
    for p in glob.glob("/usr/share/fonts/**/*.ttf", recursive=True) + glob.glob("/usr/share/fonts/**/*.otf", recursive=True):
        if os.path.basename(p) in ("DejaVuSans-Bold.ttf", "Inter-Bold.otf", "OpenSans[wdth,wght].ttf"):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()

def button_png(path, text, on):
    w, h, sc = 240, 70, 4
    im = Image.new("RGBA", (w * sc, h * sc), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    acc = (0xd3, 0x11, 0x45, 255)
    fill = acc if on else (0x2a, 0x2a, 0x2d, 255)
    d.rounded_rectangle([4 * sc, 4 * sc, (w - 4) * sc, (h - 4) * sc], radius=14 * sc, fill=fill, outline=acc, width=3 * sc)
    f = font(26 * sc)
    tw = d.textlength(text, font=f)
    d.text(((w * sc - tw) / 2, h * sc / 2), text, font=f, fill=(255, 255, 255, 255), anchor="lm")
    im.resize((w, h), Image.LANCZOS).save(path)

button_png(os.path.join(OUT, "start_off.png"), "START SYNC", False)
button_png(os.path.join(OUT, "start_on.png"), "RUNNING", True)

# Pillenschalter fuer Dry Run aus dem Standard-Kit
kit_dir, _ = g.kit_dir_for({"theme": "mono", "accent": "ffd31145"}, None, OUT)
for n in ("toggle_on.png", "toggle_off.png"):
    src = os.path.join(kit_dir, n)
    if os.path.abspath(src) != os.path.abspath(os.path.join(OUT, n)):
        import shutil; shutil.copy2(src, os.path.join(OUT, n))
for junk in glob.glob(os.path.join(OUT, "*.png")) + glob.glob(os.path.join(OUT, "kit.json")):
    if os.path.basename(junk) not in ("start_on.png", "start_off.png", "toggle_on.png", "toggle_off.png"):
        os.remove(junk)

# --- Komponenten-Definitionen ----------------------------------------------------------------
E = g.entry
def lbl(name, typ, colour, h, bounds, just="left"):
    return E(name, "Label", g.label_data(typ, colour, h, just), bounds, "No", "Show", "Always")

defs = []
# Wert zum Einstellen (Target): Name + Wert als Text, Doppeltipp = grosser Eingabe-Dialog
defs.append(("valueText", g.component_definition(g.control_actions("knob overlay"), [
    g.focus_child(S), lbl("name", "Name", S["muted"], 26.0, "15 12 248 36"), lbl("value", "Value", S["text"], 40.0, "15 50 248 50")], "00000000")))
# Reine Textanzeige (Status): kein Fokus, keine Bedienung
defs.append(("statusText", g.component_definition([], [
    lbl("name", "Name", S["muted"], 26.0, "15 12 248 36"), lbl("value", "Value", S["text"], 40.0, "15 50 248 50")], "00000000", ignore_mouse=True)))
# Zeile mit Name links und Text rechts (Name / Quelle / Ziel)
defs.append(("textLine", g.component_definition([], [
    lbl("name", "Name", S["muted"], 24.0, "15 2 200 38"), lbl("value", "Value", S["text"], 28.0, "225 2 1000 38")], "00000000", ignore_mouse=True)))
# Zeile der Jobliste: nur der Text (z. B. "> 2  Backup NAS"), das gewaehlte Ziel ist mit > markiert
defs.append(("jobLine", g.component_definition([], [
    lbl("value", "Value", S["text"], 24.0, "15 0 700 32")], "00000000", ignore_mouse=True)))
# Start-Taster: kurzer Druck, springt von selbst zurueck
btn = g.button_data("start_on.png", "start_off.png", 1, 1)
defs.append(("pushButton", g.component_definition([g.action("Mouse Down", "Q-Link", "Data", "")], [
    g.focus_child(S), E("button", "Button", btn, "19 18 240 70", "No", "Show", "Always")], "00000000")))
defs.append(("toggle", g.toggle_component(S, "menu overlay")))
defs.append(("knob overlay", g.overlay_component("KnobOverlay", g.knob_overlay_data(False), S["background"])))
defs.append(("menu overlay", g.overlay_component("MenuOverlay", g.menu_overlay_data(), S["background"])))

# --- Seite ---------------------------------------------------------------------------------------
def slot(i, c, d, handle=None, focus="Yes", inv="Hide"):
    return E(c, d, g.obj(("version", 1), ("handleName", "Data")), "%d %d %d %d" % (g.SLOT_X[i], g.SLOT_Y[0], g.SLOT_W, g.SLOT_H),
             focus, inv, "Always", remap=[("Data", handle)])

kids = [
    slot(0, "Target", "valueText", "Parameter 0"),
    E("Start Sync", "pushButton", g.obj(("version", 1), ("handleName", "Data")), "%d %d %d %d" % (g.SLOT_X[1], g.SLOT_Y[0], g.SLOT_W, g.SLOT_H),
      "Yes", "Hide", "Always", remap=[("Data", "ResetOnGestureEnd/Parameter 1")]),
    slot(2, "Status", "statusText", "Parameter 2", focus="No", inv="Show"),
    slot(3, "Dry Run", "toggle", "Parameter 3"),
]
for n, (nm, p) in enumerate([("Name", 6), ("Source", 4), ("Destination", 5), ("Progress", 15)]):
    kids.append(E(nm, "textLine", g.obj(("version", 1), ("handleName", "Data")), "23 %d 1234 42" % (170 + 44 * n),
                  "No", "Show", "Always", remap=[("Data", "Parameter %d" % p)]))

for i in range(8):
    kids.append(E("Job %d" % (i + 1), "jobLine", g.obj(("version", 1), ("handleName", "Data")), "23 %d 720 32" % (350 + 32 * i),
                  "No", "Show", "Always", remap=[("Data", "Parameter %d" % (7 + i))]))

tab = g.component_definition([], kids, S["background"])
rects = ["%d %d %d %d" % (g.SLOT_X[0] - g.QLINK_PAD, g.SLOT_Y[0] - g.QLINK_PAD,
                          g.SLOT_X[3] + g.SLOT_W + g.QLINK_PAD - (g.SLOT_X[0] - g.QLINK_PAD), g.SLOT_H + 2 * g.QLINK_PAD)]
repl = [g.obj(("key", "Parameter 2"), ("value", STATUS)), g.obj(("key", "Parameter 3"), ("value", ["Off", "On"]))]
page = g.obj(("version", 1),
             ("componentDefinitions", g.obj(("version", 2), ("importFiles", []),
                                            ("localComponentDefinitions", [g.obj(("key", k), ("value", v)) for k, v in defs]))),
             ("replacementStrings", repl),
             ("info", g.obj(("version", 1), ("type", "CompleteDescription"))),
             ("tabs", [g.obj(("version", 1), ("tabName", "SYNC"), ("fnKeyIndex", 0), ("fnKeySubIndex", 0),
                             ("componentDefinition", tab), ("qlinkBoundsData", rects))]))
tui = g.obj(("pageData", page))

qmap = g.Obj(("Q-Link %d" % q, {13: 0, 14: 1, 16: 3}.get(q, -1)) for q in range(1, 17))
prog = g.Obj(("Q-Link %d" % (q + 1), {0: 0, 1: 1, 2: 3}.get(q, -1)) for q in range(16))
ql = g.obj(("version", 4), ("info", g.obj(("version", 1), ("type", "CompleteDescription"))),
           ("Screen Mode Q-Links", g.obj(("version", 4), ("map", [g.obj(("Tab", 1), ("SubTab", 1), ("Bank Direction", "RowTopDown"), ("Q-Links", qmap))]))),
           ("Program Mode Q-Links", prog))
for name, val in (("TUI.json", tui), ("Q-Links.json", ql)):
    with open(os.path.join(OUT, name), "w", encoding="utf-8", newline="\n") as f:
        f.write(g.dump_json(val) + "\n")
print("skins/ geschrieben:", sorted(os.listdir(OUT)))
