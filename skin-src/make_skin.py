#!/usr/bin/env python3
"""Erzeugt skins/ (TUI.json, Q-Links.json, PNGs) fuer MPC Folder Sync.
Aufruf: python3 skin-src/make_skin.py   (im Projektordner)
Nutzt die Hilfsfunktionen von tools/gen_ui.py. Ergebnis wird von build.py unveraendert uebernommen."""
import os, sys, glob, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(PROJ, "..", "..", "tools"))
import gen_ui as g
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(PROJ, "skins")
os.makedirs(OUT, exist_ok=True)
S = dict(g.DEFAULT_STYLE)

ACC = (0xd3, 0x11, 0x45, 255)
GREY = (0x5a, 0x5a, 0x5c, 255)
UNCONF = "ff6a6a6c"        # gedaempfter Text fuer nicht konfigurierte Jobs
LABEL2 = "ff9a9a9c"        # Beschriftungen unten

# Layout (Koordinaten im Tab 1280 x 628)
TOP_Y, TOP_H = g.SLOT_Y[0], g.SLOT_H
TABLE_Y, ROW_PITCH, CELL_H = 316, 76, 70
NUM_W, NAME_W, BLOCK_DX, NAME_DX = 115, 486, 620, 119     # Block = Nummernkaestchen + Namenszelle

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
    fill = ACC if on else (0x2a, 0x2a, 0x2d, 255)
    d.rounded_rectangle([4 * sc, 4 * sc, (w - 4) * sc, (h - 4) * sc], radius=14 * sc, fill=fill, outline=ACC, width=3 * sc)
    f = font(26 * sc)
    tw = d.textlength(text, font=f)
    d.text(((w * sc - tw) / 2, h * sc / 2), text, font=f, fill=(255, 255, 255, 255), anchor="lm")
    im.resize((w, h), Image.LANCZOS).save(path)

def cell_png(path, w, h, on, tint):
    """Zelle der Jobtabelle: abgerundeter Rahmen mit VOLLER Innenflaeche in der Hintergrundfarbe. Die MPC nimmt Beruehrungen
    nur auf nicht durchsichtigen Pixeln an (mit durchsichtiger Innenflaeche ging der Touch nur auf der Linie). Die Texte
    liegen deshalb darueber. Aktiv: Akzentrahmen, beim Nummernkaestchen zusaetzlich volle Akzentfuellung."""
    sc = 3
    im = Image.new("RGBA", (w * sc, h * sc), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    line = ACC if on else GREY
    fill = ACC if (on and tint) else (0x1e, 0x1d, 0x1e, 255)
    d.rounded_rectangle([sc, sc, w * sc - sc - 1, h * sc - sc - 1], radius=9 * sc, fill=fill, outline=line, width=(3 if on else 2) * sc)
    im.resize((w, h), Image.LANCZOS).save(path)

button_png(os.path.join(OUT, "start_off.png"), "START SYNC", False)
button_png(os.path.join(OUT, "start_on.png"), "RUNNING", True)
for nm, w, tint in (("num", NUM_W, True), ("name", NAME_W, False)):
    cell_png(os.path.join(OUT, "cell_%s_off.png" % nm), w, CELL_H, False, tint)
    cell_png(os.path.join(OUT, "cell_%s_on.png" % nm), w, CELL_H, True, tint)

# Pillenschalter fuer Dry Run aus dem Standard-Kit
kit_dir, _ = g.kit_dir_for({"theme": "mono", "accent": "ffd31145"}, None, OUT)
for n in ("toggle_on.png", "toggle_off.png"):
    src = os.path.join(kit_dir, n)
    if os.path.abspath(src) != os.path.abspath(os.path.join(OUT, n)):
        shutil.copy2(src, os.path.join(OUT, n))
KEEP = {"start_on.png", "start_off.png", "toggle_on.png", "toggle_off.png",
        "cell_num_on.png", "cell_num_off.png", "cell_name_on.png", "cell_name_off.png"}
for junk in glob.glob(os.path.join(OUT, "*.png")) + glob.glob(os.path.join(OUT, "kit.json")):
    if os.path.basename(junk) not in KEEP:
        os.remove(junk)

# --- Komponenten-Definitionen ----------------------------------------------------------------
E = g.entry
def lbl(name, typ, colour, h, bounds, just="left"):
    return E(name, "Label", g.label_data(typ, colour, h, just), bounds, "No", "Show", "Always")

NAME_PT, VALUE_PT = 28.0, 40.0        # Obere Reihe: Beschriftung / Wert
defs = []
# Target: Nummer + Name als Text, Doppeltipp = Eingabe-Dialog
defs.append(("valueText", g.component_definition(g.control_actions("knob overlay"), [
    g.focus_child(S), lbl("name", "Name", S["muted"], NAME_PT, "15 10 330 38"), lbl("value", "Value", S["text"], VALUE_PT, "15 50 330 52")], "00000000")))
# Reine Textanzeige (Status, Progress): kein Fokus, keine Bedienung
def text_only(width):
    return g.component_definition([], [
        lbl("name", "Name", S["muted"], NAME_PT, "15 10 %d 38" % width), lbl("value", "Value", S["text"], VALUE_PT, "15 50 %d 52" % width)],
        "00000000", ignore_mouse=True)
defs.append(("statusText", text_only(190)))
defs.append(("progressText", text_only(110)))
# Dry Run: Schalter, Wert in derselben Schriftgroesse wie die anderen
defs.append(("dryToggle", g.component_definition(g.control_actions("menu overlay", double_click=False), [
    g.focus_child(S), lbl("name", "Name", S["muted"], NAME_PT, "15 10 170 38"), lbl("value", "Value", S["text"], VALUE_PT, "15 50 170 52"),
    E("toggle", "Button", g.button_data("toggle_on.png", "toggle_off.png", 1, 1), "186 20 84 68", "No", "Show", "Always")], "00000000")))
# Source / Destination
defs.append(("textLine", g.component_definition([], [
    lbl("name", "Name", LABEL2, 48.0, "15 8 270 64"), lbl("value", "Value", S["text"], 56.0, "290 0 944 76")], "00000000", ignore_mouse=True)))
# Start-Taster
btn = g.button_data("start_on.png", "start_off.png", 1, 1)
defs.append(("pushButton", g.component_definition([g.action("Mouse Down", "Q-Link", "Data", "")], [
    g.focus_child(S), E("button", "Button", btn, "11 18 240 70", "No", "Show", "Always")], "00000000")))
# Zellen der Jobtabelle: Taste (Parameter "Select Job n") mit voller Innenflaeche, wie Taster und Schalter in eine
# eigene Komponente gepackt. Antippen schaltet die Taste, das Plugin waehlt dann das Target.
for kind, w in (("num", NUM_W), ("name", NAME_W)):
    defs.append(("cell " + kind, g.component_definition([g.action("Mouse Down", "Q-Link", "Data", "")], [
        E("button", "Button", g.button_data("cell_%s_on.png" % kind, "cell_%s_off.png" % kind, 1, 1), "0 0 %d %d" % (w, CELL_H), "No", "Show", "Always")],
        "00000000")))
defs.append(("knob overlay", g.overlay_component("KnobOverlay", g.knob_overlay_data(False), S["background"])))
defs.append(("menu overlay", g.overlay_component("MenuOverlay", g.menu_overlay_data(), S["background"])))

# --- Seite ---------------------------------------------------------------------------------------
def at(name, defn, x, w, handle, focus="Yes", inv="Hide"):
    return E(name, defn, g.obj(("version", 1), ("handleName", "Data")), "%d %d %d %d" % (x, TOP_Y, w, TOP_H),
             focus, inv, "Always", remap=[("Data", handle)])

# Obere Reihe: Target | Start | Status | Progress | Dry Run
kids = [
    at("Target", "valueText", 23, 350, "Parameter 0"),
    at("Start Sync", "pushButton", 385, 262, "ResetOnGestureEnd/Parameter 1"),
    at("Status", "statusText", 655, 205, "Parameter 2", focus="No", inv="Show"),
    at("Progress", "progressText", 865, 125, "Parameter 15", focus="No", inv="Show"),
    at("Dry Run", "dryToggle", 991, 270, "Parameter 3"),
]
for n, (nm, p) in enumerate([("Source", 4), ("Destination", 5)]):
    kids.append(E(nm, "textLine", g.obj(("version", 1), ("handleName", "Data")), "23 %d 1234 76" % (164 + 76 * n),
                  "No", "Show", "Always", remap=[("Data", "Parameter %d" % p)]))

# Jobtabelle: 2 Bloecke zu je 4 Zeilen (Job 1-4 links, 5-8 rechts), je Zeile Nummernkaestchen + Name.
# Reihenfolge: erst die Zellen (Tasten), darueber die Texte.
cells = []
labels = []
for i in range(8):
    col, r = divmod(i, 4)
    x, y = 25 + BLOCK_DX * col, TABLE_Y + ROW_PITCH * r
    nx = x + NAME_DX
    labels.append(E("Job %d number" % (i + 1), "Label", g.label_data("Value", S["text"], 44.0, "horizontallyCentred verticallyCentred"),
                  "%d %d %d %d" % (x, y, NUM_W, CELL_H), "No", "Show", "Always", remap=[("Data", "Parameter %d" % (24 + i))]))
    for first, colour, tag in ((7, S["text"], "normal"), (16, S["accent"], "active"), (32, UNCONF, "unconfigured")):
        labels.append(E("Job %d name %s" % (i + 1, tag), "Label", g.label_data("Value", colour, 48.0, "left verticallyCentred"),
                      "%d %d %d %d" % (nx + 16, y, NAME_W - 28, CELL_H), "No", "Show", "Always",
                      remap=[("Data", "Parameter %d" % (first + i))]))
    for kind, cx, w in (("num", x, NUM_W), ("name", nx, NAME_W)):
        cells.append(E("Job %d %s cell" % (i + 1, kind), "cell " + kind, g.obj(("version", 1), ("handleName", "Data")),
                       "%d %d %d %d" % (cx, y, w, CELL_H), "Yes", "Show", "Always", remap=[("Data", "Parameter %d" % (24 + i))]))

kids += cells + labels      # Zellen unten, Texte darueber
tab = g.component_definition([], kids, S["background"])
rects = ["%d %d %d %d" % (13, TOP_Y - g.QLINK_PAD, 1261 + g.QLINK_PAD - 13, TOP_H + 2 * g.QLINK_PAD)]
# Zweistufige Parameter brauchen zwei Texte, damit ein Q-Link-Klick sie sicher umschaltet (sonst ist der Drehweg fein gestuft).
repl = [g.obj(("key", "Parameter 1"), ("value", ["Idle", "Start"])),
        g.obj(("key", "Parameter 3"), ("value", ["Off", "On"]))]
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
