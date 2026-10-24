#!/usr/bin/env python3
"""Derive the first-run intro's artwork masks, its captions and its geometry header.

    scripts/gen_intro_assets.py            # rewrite the outputs
    scripts/gen_intro_assets.py --check    # fail if a checked-in output is stale

Inputs
    assets/appicon.png                       the 1024 px master of the mark
    website/public/fonts/satoshi-700.woff2   the display face (headings)
    website/public/fonts/nunito-*.woff2      the body face
    website/src/data/routeMeta.js            the website's languages (checked, not
                                             read for text: the captions are below)

Outputs (all checked in; the app never reads the inputs)
    assets/intro/bunny-layers.png   RGB, three COVERAGE masks of the bunny
                                      R  silhouette (everything the mark covers)
                                      G  ink        (outline, legs, tail, face)
                                      B  face       (the `>` eye and the `_` mouth)
    assets/intro/captions-<lang>.png
                                    one per intro language: RGB, the
                                    feature captions and the skip hint, one cell
                                    each, stacked, at 2 px/pt
                                      R  primary ink   (titles, key legends)
                                      G  secondary ink (descriptions, the hint)
                                      B  key chips     (255 border, 128 body)
    src/intro/intro_geom.h          the boxes the shader and the layout need,
                                    and the list of languages

The captions' text is set by Core Text (scripts/intro_text_bake.m, built here
on the fly), so regenerating needs macOS and clang; the masks do not. Core Text
because one of the languages is Arabic and PIL cannot shape it — and because
for the scripts the display and body faces lack (they are Latin only) it falls
back to the system's faces.

Why masks and not the artwork: the mark is two flat colours on a plate, so
three coverages reproduce it exactly, and they let the intro draw the face on
its own (it opens on `>_` alone, before there is a bunny around it), recolour
it, and blur it for the glow by sampling a coarser mip. They also compress to a
fraction of the RGBA artwork, because a coverage mask is flat almost everywhere.

The captions are baked for the same reason the fonts in assets/fonts are all
monospace: the app ships no proportional face. A rendered line is an image, not
font software.
"""
import io
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFont
from scipy import ndimage as ndi

from gen_app_icon import FILL, INK, PLATE

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MASTER = os.path.join(ROOT, "assets", "appicon.png")
FONT = os.path.join(ROOT, "website", "public", "fonts", "satoshi-700.woff2")
FONT_BODY = os.path.join(ROOT, "website", "public", "fonts", "nunito-latin-var.woff2")
FONT_BODY_EXT = os.path.join(ROOT, "website", "public", "fonts", "nunito-latin-ext-var.woff2")
FONT_MONO = os.path.join(ROOT, "assets", "fonts", "JetBrainsMonoNerdFontMono-Regular.ttf")
SITE_LANGS = os.path.join(ROOT, "website", "src", "data", "routeMeta.js")
BAKER = os.path.join(ROOT, "scripts", "intro_text_bake.m")
OUT_BUNNY = os.path.join(ROOT, "assets", "intro", "bunny-layers.png")
OUT_CAPTIONS = os.path.join(ROOT, "assets", "intro", "captions-{lang}.png")
# Outputs this script removes when it finds them (and --check reports as
# stale): the single caption atlas without languages, and the "Liu" wordmark
# under the mark (the mark stands alone at the lock).
OUT_RETIRED = [os.path.join(ROOT, "assets", "intro", "captions.png"),
               os.path.join(ROOT, "assets", "intro", "wordmark.png")]
OUT_GEOM = os.path.join(ROOT, "src", "intro", "intro_geom.h")

# The three colours the master is made of live in gen_app_icon.py, which puts
# the master on its plate: unmixing against another plate than the one the
# master is on reads the whole plate as a little coverage.

MARGIN = 12          # empty border, so a clamped blurred sample fades to nothing
SNAP = 0.06          # coverage this close to 0 or 1 is codec noise, not an edge

# The features the tour presents, in order (src/intro/intro_tour.c's
# INTRO_FEATURE_*), and the chords under each: the app's DEFAULT bindings
# (src/core/keybind.c) spelled the way ui/keycap.h spells them — modifiers
# first, in HIG order — and they are the keys the tour really presses. A chip
# with a NAME in it is a palette command, typed as it is spelled: those stay in
# English in every language, because that is what the palette answers to.
CHORDS = [
    [["\u2318", "D"], ["\u21e7", "\u2318", "D"]],          # split panes
    [["\u2318", "T"]],                                      # tabs
    [["\u2318", "K"], ["Create Tab Group"]],                # tab groups
    [["\u2318", "K"]],                                      # command palette
    [["\u2318", "K"], ["Agent History"]],                   # agent history
    [["\u2318", "B"]],                                      # file browser
    [["\u21e7", "\u2318", "M"]],                            # voice prompt
    [["\u2318", "K"], ["Create Theme"]],                    # themes
]

# The intro's languages. main() checks them against the website's list
# (website/src/data/routeMeta.js), so a language added there fails here until
# it has captions. English first: it is what every other system language
# gets.
#
# This is the one place the app is not English-only: the intro's captions
# speak the reader's language. The feature names are the website's words for
# them (PAGE_LABEL in routeMeta.js), so the two agree.
LANGS = ["en", "tr", "zh", "ko", "es", "de", "it", "fr", "ja", "ar"]

# Per language: the eight titles, the eight lines under them, the skip hint.
TEXT = {
    "en": (
        ["Split panes", "Tabs", "Tab Groups", "Command Palette", "Agent History", "File Browser",
         "Voice Prompt", "Themes"],
        ["Up to eight panes in a tab, side by side or stacked.",
         "Up to 32 of them. Drag one out and it becomes a window.",
         "Name a set of tabs, give it a colour, fold it away.",
         "Every command, setting and note, one search away.",
         "Resume any session, from every agent CLI you use.",
         "Files, Markdown and code, open beside your terminal.",
         "Speak a prompt. Whisper types it, right on this Mac.",
         "Nineteen built in. Or describe one, and an agent paints it."],
        "press any key to skip"),
    "tr": (
        ["B\u00f6lmeler", "Sekmeler", "Sekme Gruplar\u0131", "Komut Paleti", "Ajan Ge\u00e7mi\u015fi",
         "Dosya Taray\u0131c\u0131", "Sesli Komut", "Temalar"],
        ["Bir sekmede sekize kadar panel; yan yana ya da alt alta.",
         "32 sekmeye kadar. Birini d\u0131\u015far\u0131 s\u00fcr\u00fckleyin, pencere olsun.",
         "Sekmeleri adland\u0131r\u0131n, renk verin, tek t\u0131kla katlay\u0131n.",
         "Her komut, ayar ve not tek bir arama uza\u011f\u0131n\u0131zda.",
         "Kulland\u0131\u011f\u0131n\u0131z her ajan CLI\u2019\u0131ndan oturumlar\u0131 s\u00fcrd\u00fcr\u00fcn.",
         "Dosyalar, Markdown ve kod; terminalinizin hemen yan\u0131nda.",
         "\u0130steminizi s\u00f6yleyin; Whisper bu Mac\u2019te yaz\u0131ya d\u00f6ker.",
         "On dokuz haz\u0131r tema. Ya da tarif edin, bir ajan boyas\u0131n."],
        "atlamak i\u00e7in bir tu\u015fa bas\u0131n"),
    "zh": (
        ["\u5206\u5c4f", "\u6807\u7b7e\u9875", "\u6807\u7b7e\u9875\u5206\u7ec4", "\u547d\u4ee4\u9762\u677f",
         "\u4ee3\u7406\u5386\u53f2\u8bb0\u5f55", "\u6587\u4ef6\u6d4f\u89c8\u5668", "\u8bed\u97f3\u63d0\u793a", "\u4e3b\u9898"],
        ["\u6bcf\u4e2a\u6807\u7b7e\u9875\u6700\u591a\u516b\u4e2a\u7a97\u683c\uff0c\u53ef\u5de6\u53f3\u6216\u4e0a\u4e0b\u6392\u5217\u3002",
         "\u6700\u591a 32 \u4e2a\u6807\u7b7e\u9875\u3002\u62d6\u51fa\u4e00\u4e2a\uff0c\u5b83\u5c31\u6210\u4e3a\u65b0\u7a97\u53e3\u3002",
         "\u4e3a\u4e00\u7ec4\u6807\u7b7e\u9875\u547d\u540d\u3001\u4e0a\u8272\uff0c\u4e00\u952e\u6298\u53e0\u3002",
         "\u6240\u6709\u547d\u4ee4\u3001\u8bbe\u7f6e\u548c\u7b14\u8bb0\uff0c\u4e00\u641c\u5373\u8fbe\u3002",
         "\u7ee7\u7eed\u4efb\u610f\u4f1a\u8bdd\uff0c\u6765\u81ea\u4f60\u4f7f\u7528\u7684\u6bcf\u4e2a\u4ee3\u7406 CLI\u3002",
         "\u6587\u4ef6\u3001Markdown \u4e0e\u4ee3\u7801\uff0c\u5c31\u5728\u7ec8\u7aef\u65c1\u6253\u5f00\u3002",
         "\u8bf4\u51fa\u63d0\u793a\u8bcd\uff0cWhisper \u5728\u672c\u673a\u4e3a\u4f60\u8f93\u5165\u3002",
         "\u5185\u7f6e\u5341\u4e5d\u6b3e\u4e3b\u9898\u3002\u6216\u63cf\u8ff0\u4e00\u6b3e\uff0c\u7531\u4ee3\u7406\u4e3a\u4f60\u7ed8\u5236\u3002"],
        "\u6309\u4efb\u610f\u952e\u8df3\u8fc7"),
    "ko": (
        ["\ubd84\ud560", "\ud0ed", "\ud0ed \uadf8\ub8f9", "\uba85\ub839 \ud314\ub808\ud2b8", "\uc5d0\uc774\uc804\ud2b8 \uae30\ub85d",
         "\ud30c\uc77c \ud0d0\uc0c9\uae30", "\uc74c\uc131 \ud504\ub86c\ud504\ud2b8", "\ud14c\ub9c8"],
        ["\ud0ed \ud558\ub098\uc5d0 \ucd5c\ub300 8\uac1c\uc758 \ucc3d\uc744 \uc88c\uc6b0 \ub610\ub294 \uc0c1\ud558\ub85c.",
         "\ucd5c\ub300 32\uac1c\uc758 \ud0ed. \ub04c\uc5b4\ub0b4\uba74 \uc0c8 \ucc3d\uc774 \ub429\ub2c8\ub2e4.",
         "\ud0ed\uc744 \ubb36\uc5b4 \uc774\ub984\uacfc \uc0c9\uc744 \uc815\ud558\uace0, \uc811\uc5b4 \ub450\uc138\uc694.",
         "\ubaa8\ub4e0 \uba85\ub839, \uc124\uc815, \ub178\ud2b8\ub97c \ud55c \ubc88\uc758 \uac80\uc0c9\uc73c\ub85c.",
         "\uc0ac\uc6a9\ud558\ub294 \ubaa8\ub4e0 \uc5d0\uc774\uc804\ud2b8 CLI\uc758 \uc138\uc158\uc744 \uc774\uc5b4\uc11c.",
         "\ud30c\uc77c, Markdown, \ucf54\ub4dc\ub97c \ud130\ubbf8\ub110 \ubc14\ub85c \uc606\uc5d0\uc11c.",
         "\ub9d0\ud558\uba74 Whisper\uac00 \uc774 Mac\uc5d0\uc11c \ubc14\ub85c \uc785\ub825\ud569\ub2c8\ub2e4.",
         "19\uac00\uc9c0 \uae30\ubcf8 \uc81c\uacf5. \uc124\uba85\ud558\uba74 \uc5d0\uc774\uc804\ud2b8\uac00 \uadf8\ub824 \uc90d\ub2c8\ub2e4."],
        "\uc544\ubb34 \ud0a4\ub098 \ub20c\ub7ec \uac74\ub108\ub6f0\uae30"),
    "es": (
        ["Divisiones", "Pesta\u00f1as", "Grupos de pesta\u00f1as", "Paleta de comandos", "Historial de agentes",
         "Explorador de archivos", "Prompt por voz", "Temas"],
        ["Hasta ocho paneles por pesta\u00f1a, en paralelo o apilados.",
         "Hasta 32 pesta\u00f1as. Arrastra una fuera y ser\u00e1 una ventana.",
         "Nombra un grupo de pesta\u00f1as, dale color y pli\u00e9galo.",
         "Cada comando, ajuste y nota, a una b\u00fasqueda de distancia.",
         "Reanuda cualquier sesi\u00f3n, de cada CLI de agente que uses.",
         "Archivos, Markdown y c\u00f3digo, abiertos junto a tu terminal.",
         "Dicta un prompt. Whisper lo escribe, en este mismo Mac.",
         "Diecinueve incluidos. O describe uno y un agente lo pinta."],
        "pulsa cualquier tecla para omitir"),
    "de": (
        ["Aufteilungen", "Tabs", "Tab-Gruppen", "Befehlspalette", "Agenten-Historie", "Dateibrowser",
         "Sprach-Prompt", "Themes"],
        ["Bis zu acht Bereiche pro Tab, neben- oder \u00fcbereinander.",
         "Bis zu 32 Tabs. Einen herausziehen, und er wird ein Fenster.",
         "Tabs benennen, einf\u00e4rben und zusammenklappen.",
         "Jeder Befehl, jede Einstellung, jede Notiz: eine Suche entfernt.",
         "Jede Sitzung fortsetzen, aus jeder genutzten Agent-CLI.",
         "Dateien, Markdown und Code, direkt neben dem Terminal.",
         "Prompt einsprechen. Whisper tippt ihn, direkt auf diesem Mac.",
         "Neunzehn eingebaut. Oder eins beschreiben, ein Agent malt es."],
        "beliebige Taste zum \u00dcberspringen"),
    "it": (
        ["Divisioni", "Schede", "Gruppi di schede", "Tavolozza comandi", "Cronologia agenti", "Browser file",
         "Prompt vocale", "Temi"],
        ["Fino a otto pannelli per scheda, affiancati o impilati.",
         "Fino a 32 schede. Trascinane una fuori e diventa una finestra.",
         "Dai un nome a un gruppo di schede, un colore, e ripiegalo.",
         "Ogni comando, impostazione e nota a una ricerca di distanza.",
         "Riprendi qualsiasi sessione, da ogni CLI di agenti che usi.",
         "File, Markdown e codice, aperti accanto al terminale.",
         "Detta un prompt. Whisper lo scrive, su questo Mac.",
         "Diciannove inclusi. O descrivine uno, e un agente lo dipinge."],
        "premi un tasto per saltare"),
    "fr": (
        ["Divisions", "Onglets", "Groupes d\u2019onglets", "Palette de commandes", "Historique des agents",
         "Explorateur de fichiers", "Prompt vocal", "Th\u00e8mes"],
        ["Jusqu\u2019\u00e0 huit volets par onglet, c\u00f4te \u00e0 c\u00f4te ou empil\u00e9s.",
         "Jusqu\u2019\u00e0 32 onglets. Faites-en glisser un : il devient une fen\u00eatre.",
         "Nommez un ensemble d\u2019onglets, colorez-le, repliez-le.",
         "Chaque commande, r\u00e9glage et note \u00e0 une recherche pr\u00e8s.",
         "Reprenez n\u2019importe quelle session, de chaque CLI d\u2019agent.",
         "Fichiers, Markdown et code, ouverts \u00e0 c\u00f4t\u00e9 du terminal.",
         "Dictez un prompt. Whisper le saisit, sur ce Mac.",
         "Dix-neuf int\u00e9gr\u00e9s. Ou d\u00e9crivez-en un, un agent le peint."],
        "appuyez sur une touche pour passer"),
    "ja": (
        ["\u5206\u5272", "\u30bf\u30d6", "\u30bf\u30d6\u30b0\u30eb\u30fc\u30d7", "\u30b3\u30de\u30f3\u30c9\u30d1\u30ec\u30c3\u30c8",
         "\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u5c65\u6b74", "\u30d5\u30a1\u30a4\u30eb\u30d6\u30e9\u30a6\u30b6", "\u97f3\u58f0\u30d7\u30ed\u30f3\u30d7\u30c8", "\u30c6\u30fc\u30de"],
        ["1\u3064\u306e\u30bf\u30d6\u306b\u6700\u59278\u30da\u30a4\u30f3\u3002\u5de6\u53f3\u306b\u3082\u4e0a\u4e0b\u306b\u3082\u5206\u5272\u3002",
         "\u6700\u592732\u30bf\u30d6\u3002\u30c9\u30e9\u30c3\u30b0\u3067\u5916\u306b\u51fa\u305b\u3070\u3001\u30a6\u30a3\u30f3\u30c9\u30a6\u306b\u3002",
         "\u30bf\u30d6\u3092\u307e\u3068\u3081\u3066\u540d\u524d\u3068\u8272\u3092\u4ed8\u3051\u3001\u6298\u308a\u305f\u305f\u3081\u307e\u3059\u3002",
         "\u3059\u3079\u3066\u306e\u30b3\u30de\u30f3\u30c9\u3001\u8a2d\u5b9a\u3001\u30ce\u30fc\u30c8\u3092\u3001\u3072\u3068\u3064\u306e\u691c\u7d22\u3067\u3002",
         "\u3069\u306e\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8CLI\u306e\u30bb\u30c3\u30b7\u30e7\u30f3\u3082\u3001\u3059\u3050\u306b\u518d\u958b\u3002",
         "\u30d5\u30a1\u30a4\u30eb\u3001Markdown\u3001\u30b3\u30fc\u30c9\u3092\u3001\u30bf\u30fc\u30df\u30ca\u30eb\u306e\u96a3\u3067\u3002",
         "\u8a71\u3059\u3060\u3051\u3002Whisper\u304c\u3053\u306eMac\u4e0a\u3067\u6587\u5b57\u306b\u3057\u307e\u3059\u3002",
         "19\u7a2e\u985e\u3092\u5185\u8535\u3002\u8aac\u660e\u3059\u308c\u3070\u3001\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u304c\u4f5c\u308a\u307e\u3059\u3002"],
        "\u4efb\u610f\u306e\u30ad\u30fc\u3067\u30b9\u30ad\u30c3\u30d7"),
    "ar": (
        ["\u0627\u0644\u062a\u0642\u0633\u064a\u0645\u0627\u062a", "\u0639\u0644\u0627\u0645\u0627\u062a \u0627\u0644\u062a\u0628\u0648\u064a\u0628",
         "\u0645\u062c\u0645\u0648\u0639\u0627\u062a \u0639\u0644\u0627\u0645\u0627\u062a \u0627\u0644\u062a\u0628\u0648\u064a\u0628", "\u0644\u0648\u062d\u0629 \u0627\u0644\u0623\u0648\u0627\u0645\u0631",
         "\u0633\u062c\u0644 \u0627\u0644\u0648\u0643\u064a\u0644", "\u0645\u062a\u0635\u0641\u062d \u0627\u0644\u0645\u0644\u0641\u0627\u062a", "\u0645\u0648\u062c\u0651\u0647 \u0635\u0648\u062a\u064a",
         "\u0627\u0644\u0645\u0638\u0647\u0631"],
        ["\u062d\u062a\u0649 \u062b\u0645\u0627\u0646\u064a\u0629 \u0623\u062c\u0632\u0627\u0621 \u0641\u064a \u0627\u0644\u062a\u0628\u0648\u064a\u0628\u060c \u0645\u062a\u062c\u0627\u0648\u0631\u0629 \u0623\u0648 \u0645\u062a\u0631\u0627\u0635\u0651\u0629.",
         "\u062d\u062a\u0649 32 \u062a\u0628\u0648\u064a\u0628\u064b\u0627. \u0627\u0633\u062d\u0628 \u0648\u0627\u062d\u062f\u064b\u0627 \u0644\u0644\u062e\u0627\u0631\u062c \u0644\u064a\u0635\u0628\u062d \u0646\u0627\u0641\u0630\u0629.",
         "\u0633\u0645\u0651\u0650 \u0645\u062c\u0645\u0648\u0639\u0629 \u062a\u0628\u0648\u064a\u0628\u0627\u062a\u060c \u0644\u0648\u0651\u0646\u0647\u0627\u060c \u062b\u0645 \u0627\u0637\u0648\u0650\u0647\u0627.",
         "\u0643\u0644 \u0623\u0645\u0631 \u0648\u0625\u0639\u062f\u0627\u062f \u0648\u0645\u0644\u0627\u062d\u0638\u0629 \u0639\u0644\u0649 \u0628\u064f\u0639\u062f \u0628\u062d\u062b \u0648\u0627\u062d\u062f.",
         "\u0627\u0633\u062a\u0623\u0646\u0641 \u0623\u064a \u062c\u0644\u0633\u0629\u060c \u0645\u0646 \u0643\u0644 \u0648\u0627\u062c\u0647\u0629 CLI \u0644\u0644\u0648\u0643\u0644\u0627\u0621 \u062a\u0633\u062a\u062e\u062f\u0645\u0647\u0627.",
         "\u0627\u0644\u0645\u0644\u0641\u0627\u062a \u0648 Markdown \u0648\u0627\u0644\u0643\u0648\u062f\u060c \u0628\u062c\u0648\u0627\u0631 \u0627\u0644\u0637\u0631\u0641\u064a\u0629.",
         "\u0627\u0646\u0637\u0642 \u0637\u0644\u0628\u0643\u060c \u0648 Whisper \u064a\u0643\u062a\u0628\u0647 \u0639\u0644\u0649 \u0647\u0630\u0627 \u0627\u0644\u0640 Mac.",
         "\u062a\u0633\u0639\u0629 \u0639\u0634\u0631 \u0645\u0638\u0647\u0631\u064b\u0627 \u062c\u0627\u0647\u0632\u064b\u0627. \u0623\u0648 \u0635\u0650\u0641 \u0648\u0627\u062d\u062f\u064b\u0627 \u0644\u064a\u0631\u0633\u0645\u0647 \u0648\u0643\u064a\u0644."],
        "\u0627\u0636\u063a\u0637 \u0623\u064a \u0645\u0641\u062a\u0627\u062d \u0644\u0644\u062a\u062e\u0637\u064a"),
}

# The scripts the Latin-only display and body faces do not have: which
# system face stands in (the title's, the body's), and the tag Core Text is
# told — it decides which regional form a shared code point takes. Named
# faces are macOS's own.
SF_ARABIC = "/System/Library/Fonts/SFArabic.ttf"
SCRIPT_FACES = {
    "zh": ("name:PingFangSC-Semibold", "name:PingFangSC-Regular", "zh-Hans"),
    "ja": ("name:HiraginoSans-W6", "name:HiraginoSans-W4", "ja"),
    "ko": ("name:AppleSDGothicNeo-Bold", "name:AppleSDGothicNeo-Medium", "ko"),
    "ar": ((SF_ARABIC + "@700") if os.path.exists(SF_ARABIC) else "name:GeezaPro-Bold",
           (SF_ARABIC + "@500") if os.path.exists(SF_ARABIC) else "name:GeezaPro", "ar"),
}
CAPTION_SCALE = 2
CAPTION_W = 520      # points
CAPTION_H = 100


def unmix(rgb):
    """Each pixel as a mix of plate, fill and ink; returns (fill, ink) weights."""
    basis = np.stack([FILL - PLATE, INK - PLATE], axis=1)          # 3 x 2
    w = (rgb - PLATE) @ np.linalg.pinv(basis).T                     # H x W x 2
    w = np.clip(w, 0.0, 1.0)
    total = w.sum(-1, keepdims=True)
    w = np.where(total > 1.0, w / np.maximum(total, 1e-6), w)
    return w[..., 0], w[..., 1]


def build_bunny():
    rgb = np.asarray(Image.open(MASTER).convert("RGB")).astype(float)
    fill, ink = unmix(rgb)
    sil = np.clip(fill + ink, 0.0, 1.0)

    # The plate is not perfectly flat (the master went through a lossy codec at
    # some point), so anything not attached to the mark is noise: keep only the
    # largest solid component, grown by a few pixels to hold its soft edge.
    solid = sil > 0.5
    labels, count = ndi.label(solid)
    if count == 0:
        raise SystemExit("no mark found in the master")
    sizes = ndi.sum(solid, labels, range(1, count + 1))
    body = labels == (1 + int(np.argmax(sizes)))
    keep = ndi.binary_dilation(body, iterations=4)
    sil = np.where(keep, sil, 0.0)
    ink = np.where(keep, np.minimum(ink, sil), 0.0)

    # The face: ink components that do not touch the outline. Exactly two are
    # expected, the `>` on the left and the `_` on the right.
    ink_solid = ink > 0.5
    labels, count = ndi.label(ink_solid)
    sizes = ndi.sum(ink_solid, labels, range(1, count + 1))
    order = np.argsort(-sizes)
    boxes = ndi.find_objects(labels)
    parts = []
    for index in order[1:]:
        if sizes[index] < 400:
            continue
        parts.append((index + 1, boxes[index]))
    if len(parts) != 2:
        raise SystemExit(f"expected 2 face strokes, found {len(parts)}")
    parts.sort(key=lambda part: part[1][1].start)
    face_mask = np.zeros_like(ink_solid)
    for label, _ in parts:
        face_mask |= ndi.binary_dilation(labels == label, iterations=5)
    face = np.where(face_mask, ink, 0.0)

    ys, xs = np.where(body)
    x0, x1 = xs.min() - MARGIN, xs.max() + 1 + MARGIN
    y0, y1 = ys.min() - MARGIN, ys.max() + 1 + MARGIN
    # Even dimensions keep every mip level's texel grid aligned with the last.
    x1 += (x1 - x0) % 2
    y1 += (y1 - y0) % 2

    def crop(layer):
        # Snap the nearly-empty and nearly-full values. The master's flat areas
        # carry a couple of levels of codec noise, which is invisible on screen
        # and is most of what a PNG of these masks would otherwise spend its
        # bytes on (227 KB unsnapped).
        layer = layer[y0:y1, x0:x1]
        layer = np.where(layer < SNAP, 0.0, np.where(layer > 1.0 - SNAP, 1.0, layer))
        return np.round(layer * 255.0).astype(np.uint8)

    image = Image.fromarray(np.dstack([crop(sil), crop(ink), crop(face)]), "RGB")

    def box(part):
        ys_, xs_ = part[1]
        return (xs_.start - x0, ys_.start - y0, xs_.stop - x0, ys_.stop - y0)

    eye, mouth = box(parts[0]), box(parts[1])
    mark = (xs.min() - x0, ys.min() - y0, xs.max() + 1 - x0, ys.max() + 1 - y0)
    geom = {
        "size": (x1 - x0, y1 - y0),
        "mark": mark,
        "eye": eye,
        "mouth": mouth,
        "split": (eye[2] + mouth[0]) / 2.0,
    }
    # How far the fill has to travel from the face to cover the whole mark.
    cx = (eye[0] + mouth[2]) / 2.0
    cy = (eye[1] + eye[3]) / 2.0
    by, bx = np.where(body[y0:y1, x0:x1])
    geom["face_center"] = (cx, cy)
    geom["reach"] = float(np.sqrt((bx - cx) ** 2 + (by - cy) ** 2).max())
    return image, geom


def _ttf(path, size, weight=None):
    """A PIL face from a .ttf or a .woff2 (which FreeType cannot open itself)."""
    if path.endswith(".woff2"):
        from fontTools.ttLib import TTFont

        font = TTFont(path)
        font.flavor = None
        data = io.BytesIO()
        font.save(data)
        data.seek(0)
        face = ImageFont.truetype(data, size)
    else:
        face = ImageFont.truetype(path, size)
    if weight is not None:
        face.set_variation_by_axes([weight])
    return face


def _plain_ttf(path, workdir):
    """A .ttf Core Text can open, from a .woff2 it cannot."""
    if not path.endswith(".woff2"):
        return path
    from fontTools.ttLib import TTFont

    font = TTFont(path)
    font.flavor = None
    out = os.path.join(workdir, os.path.basename(path)[:-6] + ".ttf")
    font.save(out)
    return out


def _cmap(path):
    from fontTools.ttLib import TTFont

    return set(TTFont(path).getBestCmap())


class TextBaker:
    """Compile the Core Text helper used to rasterize intro captions."""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="liu-intro-bake-")
        self.tool = os.path.join(self.dir, "intro_text_bake")
        subprocess.run(["clang", "-fobjc-arc", "-O1", "-o", self.tool, BAKER, "-framework", "Foundation",
                        "-framework", "CoreText", "-framework", "CoreGraphics"], check=True)
        self.title = _plain_ttf(FONT, self.dir)
        self.body = _plain_ttf(FONT_BODY, self.dir) + "@560"
        self.body_ext = _plain_ttf(FONT_BODY_EXT, self.dir) + "@560"
        self.mono = FONT_MONO

    def run(self, jobs):
        """jobs: {id: (font, cascade, size px, tracking px, language, text)}.
        Returns {id: (coverage image, baseline from its top, advance, faces used)}."""
        listing = os.path.join(self.dir, "jobs.tsv")
        with open(listing, "w", encoding="utf-8") as handle:
            for ident, (font, cascade, size, tracking, language, text) in jobs.items():
                assert "\t" not in text and "\n" not in text
                handle.write("\t".join([ident, font, cascade or "-", f"{size:.4f}", f"{tracking:.4f}",
                                        language, text]) + "\n")
        done = subprocess.run([self.tool, listing, self.dir], check=True, capture_output=True, text=True)
        tiles = {}
        for line in done.stdout.splitlines():
            ident, width, height, baseline, advance, faces = line.split(" ", 5)
            with open(os.path.join(self.dir, ident + ".gray"), "rb") as handle:
                image = Image.frombytes("L", (int(width), int(height)), handle.read())
            tiles[ident] = (image, float(baseline), float(advance), faces)
        return tiles


# Where the baker pads its tiles (kPad in intro_text_bake.m).
BAKE_PAD = 6


def caption_jobs(baker, lang, scale=None):
    """The lines of one language's atlas, as baker jobs. `scale` shrinks a line
    that did not fit: {id: factor}."""
    k = CAPTION_SCALE
    titles, lines, hint = TEXT[lang]
    script = SCRIPT_FACES.get(lang)
    tag = script[2] if script else lang
    # Tracking is a Latin habit: it opens up CJK needlessly and pulls Arabic's
    # joined letters apart.
    track = 0.0 if script else 1.0
    title_cascade = script[0] if script else None
    body_cascade = baker.body_ext + ("|" + script[1] if script else "")
    jobs = {}

    def add(ident, font, cascade, size, tracking, text):
        factor = (scale or {}).get(ident, 1.0)
        jobs[ident] = (font, cascade, size * factor, tracking * factor, tag, text)

    for index, (name, line) in enumerate(zip(titles, lines)):
        add(f"{lang}-t{index}", baker.title, title_cascade, 26 * k, -0.02 * 26 * k * track, name)
        add(f"{lang}-d{index}", baker.body, body_cascade, 14 * k, 0.0, line)
    # The caption hint uses the terminal font when it supports the script.
    # Otherwise it uses the body font to avoid spacing words with full-width
    # monospaced spaces from another face.
    if script:
        add(f"{lang}-h", baker.body, body_cascade, 11 * k, 0.0, hint)
    else:
        add(f"{lang}-h", baker.mono, None, 11 * k, 0.04 * 11 * k, hint)
    return jobs


def check_coverage(baker):
    """In the Latin languages every letter must come from the display and body
    faces: one that fell through to a system face would be a different typeface
    in the middle of a word."""
    title = _cmap(baker.title)
    body = _cmap(baker.body.split("@")[0]) | _cmap(baker.body_ext.split("@")[0])
    mono = _cmap(baker.mono)
    for lang in LANGS:
        if lang in SCRIPT_FACES:
            continue
        titles, lines, hint = TEXT[lang]
        for text, cmap, face in [(t, title, "Satoshi") for t in titles] + [(d, body, "Nunito") for d in lines] \
                + [(hint, mono, "JetBrains Mono")]:
            missing = sorted({ch for ch in text if ord(ch) not in cmap})
            if missing:
                raise SystemExit(f"{lang}: {face} has no {missing!r} (in {text!r})")


def build_captions(baker):
    """One atlas per language. Returns ({lang: image}, geometry)."""
    k = CAPTION_SCALE
    cell_w, cell_h = CAPTION_W * k, CAPTION_H * k
    count = len(CHORDS)
    cells = count + 1
    check_coverage(baker)

    # Set everything once; whatever is wider than its cell allows is set again,
    # smaller, rather than squeezed.
    limit = {"t": (CAPTION_W - 36) * k, "d": (CAPTION_W - 32) * k, "h": (CAPTION_W - 20) * k}
    jobs = {}
    for lang in LANGS:
        assert len(TEXT[lang][0]) == count and len(TEXT[lang][1]) == count, lang
        jobs.update(caption_jobs(baker, lang))
    tiles = baker.run(jobs)
    shrink = {}
    for ident, (_, _, advance, _) in tiles.items():
        kind = ident.split("-")[1][0]
        if advance > limit[kind]:
            shrink[ident] = limit[kind] / advance * 0.995
    if shrink:
        again = {}
        for lang in LANGS:
            again.update({i: j for i, j in caption_jobs(baker, lang, shrink).items() if i in shrink})
        tiles.update(baker.run(again))

    # The baselines the PIL-set English atlas had, so the cells did not move
    # when the text's rasteriser changed.
    title_base = 2 * k + _ttf(FONT, 26 * k).getmetrics()[0]
    body_base = 41 * k + _ttf(FONT_BODY, 14 * k, weight=560).getmetrics()[0]
    hint_base = 40 * k + _ttf(FONT_MONO, 11 * k).getmetrics()[0]
    mono = _ttf(FONT_MONO, 13 * k)
    chip_h, chip_pad, key_gap, chord_gap = 24 * k, 7 * k, 3 * k, 16 * k

    def place(layer, ident, baseline):
        tile, tile_base, advance, _ = tiles[ident]
        x = round((cell_w - advance) / 2.0) - BAKE_PAD
        y = round(baseline - tile_base)
        box = (x, y, x + tile.size[0], y + tile.size[1])
        layer.paste(ImageChops.lighter(layer.crop(box), tile), box)

    atlases = {}
    for lang in LANGS:
        primary = Image.new("L", (cell_w, cell_h * cells), 0)
        secondary = Image.new("L", (cell_w, cell_h * cells), 0)
        chips = Image.new("L", (cell_w, cell_h * cells), 0)
        d1, d3 = ImageDraw.Draw(primary), ImageDraw.Draw(chips)
        for index, chords in enumerate(CHORDS):
            top = index * cell_h
            place(primary, f"{lang}-t{index}", top + title_base)
            place(secondary, f"{lang}-d{index}", top + body_base)

            # One flat chip per key, a chord's keys 3 pt apart. Left to
            # right in every language: they are keys.
            sizes = [[max(mono.getlength(key) + 2 * chip_pad, chip_h) for key in chord]
                     for chord in chords]
            total = sum(sum(row) + key_gap * (len(row) - 1) for row in sizes) \
                + chord_gap * (len(chords) - 1)
            x = (cell_w - total) / 2.0
            y = top + 71 * k
            for chord, row in zip(chords, sizes):
                for key, width in zip(chord, row):
                    box = (round(x), y, round(x + width) - 1, y + chip_h - 1)
                    d3.rounded_rectangle(box, radius=5 * k, fill=128, outline=255, width=k // 2 or 1)
                    left, upper, right, lower = mono.getbbox(key)
                    d1.text((x + (width - (right - left)) / 2.0 - left,
                             y + (chip_h - (lower - upper)) / 2.0 - upper), key, font=mono, fill=255)
                    x += width + key_gap
                x += chord_gap - key_gap
        place(secondary, f"{lang}-h", count * cell_h + hint_base)
        atlases[lang] = Image.merge("RGB", (primary, secondary, chips))

    faces = {}
    for ident, (_, _, _, used) in tiles.items():
        faces.setdefault(ident.split("-")[0], set()).update(used.split(","))
    return atlases, {"cell": (cell_w, cell_h), "count": count, "hints": 1, "scale": k,
                     "shrunk": shrink, "faces": faces}


def site_languages():
    """The language codes the site lists, in its order; None if the site is not here."""
    if not os.path.exists(SITE_LANGS):
        return None
    with open(SITE_LANGS, encoding="utf-8") as handle:
        source = handle.read()
    block = source[source.index("export const languages = ["):]
    block = block[:block.index("];")]
    return re.findall(r"code:\s*'([a-z]+)'", block)


def geom_header(bunny, captions):
    def f(value):
        return f"{float(value):.1f}f"

    mx0, my0, mx1, my1 = bunny["mark"]
    ex0, ey0, ex1, ey1 = bunny["eye"]
    ux0, uy0, ux1, uy1 = bunny["mouth"]
    lines = [
        "/* GENERATED by scripts/gen_intro_assets.py — do not edit by hand.",
        " *",
        " * Geometry of the files under assets/intro/ (bunny-layers, the caption atlases),",
        " * in the pixels of those files (origin top-left, y down). The shader in",
        " * intro_render_macos.m is handed these as uniforms; the layout in",
        " * intro_timeline.c sizes the lockup from them. */",
        "#ifndef LIU_INTRO_GEOM_H",
        "#define LIU_INTRO_GEOM_H",
        "",
        f"#define INTRO_BUNNY_W        {f(bunny['size'][0])}",
        f"#define INTRO_BUNNY_H        {f(bunny['size'][1])}",
        "/* The mark's own box inside the file (the file adds an empty margin). */",
        f"#define INTRO_MARK_X0        {f(mx0)}",
        f"#define INTRO_MARK_Y0        {f(my0)}",
        f"#define INTRO_MARK_X1        {f(mx1)}",
        f"#define INTRO_MARK_Y1        {f(my1)}",
        "/* The `>` of the face. */",
        f"#define INTRO_EYE_X0         {f(ex0)}",
        f"#define INTRO_EYE_Y0         {f(ey0)}",
        f"#define INTRO_EYE_X1         {f(ex1)}",
        f"#define INTRO_EYE_Y1         {f(ey1)}",
        "/* The `_` of the face: the cursor the intro opens and closes on. */",
        f"#define INTRO_MOUTH_X0       {f(ux0)}",
        f"#define INTRO_MOUTH_Y0       {f(uy0)}",
        f"#define INTRO_MOUTH_X1       {f(ux1)}",
        f"#define INTRO_MOUTH_Y1       {f(uy1)}",
        "/* The column between them: left of it the face mask is the eye. */",
        f"#define INTRO_FACE_SPLIT_X   {f(bunny['split'])}",
        "/* Where `>_` is centred, and how far the fill travels from there. */",
        f"#define INTRO_FACE_CX        {f(bunny['face_center'][0])}",
        f"#define INTRO_FACE_CY        {f(bunny['face_center'][1])}",
        f"#define INTRO_FLOOD_REACH    {f(bunny['reach'])}",
        "",
        "/* assets/intro/captions-<lang>.png: INTRO_CAPTION_COUNT feature captions and",
        " * then INTRO_CAPTION_HINTS hint lines, one cell each, stacked top to bottom.",
        " * A cell is INTRO_CAPTION_SCALE pixels per point. */",
        f"#define INTRO_CAPTION_COUNT  {captions['count']}",
        f"#define INTRO_CAPTION_HINTS  {captions['hints']}",
        f"#define INTRO_CAPTION_CELL_W {f(captions['cell'][0])}",
        f"#define INTRO_CAPTION_CELL_H {f(captions['cell'][1])}",
        f"#define INTRO_CAPTION_SCALE  {f(captions['scale'])}",
        "",
        "/* The languages there is a caption atlas for — the site's — as their",
        " * primary subtags, English first. src/intro/intro_lang.c matches the",
        " * system's language against these; CMakeLists.txt embeds one atlas each. */",
        f"#define INTRO_LANG_COUNT     {len(LANGS)}",
        "#define INTRO_LANG_CODES     { " + ", ".join(f'"{code}"' for code in LANGS) + " }",
        "",
        "#endif /* LIU_INTRO_GEOM_H */",
        "",
    ]
    return "\n".join(lines)


def png_bytes(image):
    out = io.BytesIO()
    image.save(out, "PNG", optimize=True)
    return out.getvalue()


def main():
    check = "--check" in sys.argv[1:]
    site = site_languages()
    if site is not None and site != LANGS:
        print(f"the site is in {site}, the intro's captions in {LANGS}: add the missing "
              f"language to TEXT (and to LANGS, in the site's order)", file=sys.stderr)
        return 1
    bunny_image, bunny = build_bunny()
    caption_images, captions = build_captions(TextBaker())
    outputs = {
        OUT_BUNNY: png_bytes(bunny_image),
        OUT_GEOM: geom_header(bunny, captions).encode(),
    }
    for lang, image in caption_images.items():
        outputs[OUT_CAPTIONS.format(lang=lang)] = png_bytes(image)
    stale = []
    for path, data in outputs.items():
        current = open(path, "rb").read() if os.path.exists(path) else None
        if current == data:
            continue
        if check:
            stale.append(path)
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(data)
        print(f"wrote {os.path.relpath(path, ROOT)} ({len(data)} bytes)")
    for path in OUT_RETIRED:
        if not os.path.exists(path):
            continue
        if check:
            stale.append(path)
        else:
            os.remove(path)
            print(f"removed {os.path.relpath(path, ROOT)}")
    if stale:
        for path in stale:
            print(f"stale: {os.path.relpath(path, ROOT)}", file=sys.stderr)
        return 1
    print(f"bunny {bunny['size']} mark {bunny['mark']} eye {bunny['eye']} "
          f"mouth {bunny['mouth']} reach {bunny['reach']:.1f}")
    for lang in LANGS:
        shrunk = sorted(i for i in captions["shrunk"] if i.startswith(lang + "-"))
        print(f"captions {lang}: faces {', '.join(sorted(captions['faces'][lang]))}"
              + (f"; set smaller to fit: {', '.join(shrunk)}" if shrunk else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
