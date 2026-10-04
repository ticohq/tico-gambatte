#!/usr/bin/env python3
"""Settings definition and translations for the tico module of gambatte.

Run after merging upstream. It builds tools/dump_core_options.c against the
core's own libretro_core_options.h, so tico/module/settings.json lists exactly
the options the libnx core reads (gambatte_* keys, values and defaults), laid
out in tabs, followed by the overlay's own display options. Labels are
translation keys; the strings go into tico/lang/*.json, taken from tico's
existing settings labels where an option already had one and otherwise from
the core's own translations. Choice labels stay English in settings.json; the
overlay translates them through settings_gambatte_value_* keys.

    python3 tico/tools/tico_module.py
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TICO = ROOT / "tico"
SETTINGS = TICO / "module/settings.json"
LANG_DIR = TICO / "lang"
LANGUAGES = ("en", "de", "es", "fr", "ja", "pt", "ru", "zh")
# tico-nx's own strings label the options it already knew; optional
TICO_NX = Path(os.environ.get("TICO_NX_DIR", ROOT.parents[1] / "tico-nx"))

# Core option -> label key. Options tico already labelled keep their key (and
# tico's translations); the rest are named after the option.
LABEL_KEYS = {
    "gambatte_gb_hwmode": "settings_gambatte_hardware_mode",
    "gambatte_gb_bootloader": "settings_gambatte_official_bootloader",
    "gambatte_gb_colorization": "settings_gambatte_colorization_mode",
    "gambatte_gb_internal_palette": "settings_gambatte_internal_palette",
    "gambatte_gbc_color_correction": "settings_gambatte_correction",
    "gambatte_gbc_color_correction_mode": "settings_gambatte_correction_mode",
    "gambatte_gbc_frontlight_position": "settings_gambatte_frontlight_position",
    "gambatte_dark_filter_level": "settings_gambatte_dark_filter_level",
    "gambatte_mix_frames": "settings_gambatte_ghosting_blending",
    "gambatte_audio_resampler": "settings_gambatte_resampler_quality",
    "gambatte_up_down_allowed": "settings_gambatte_allow_opposing_dir",
    "gambatte_turbo_period": "settings_gambatte_turbo_period",
}

# (tab, [(section, [option keys])]). Every core option the dump lists must be
# placed; the overlay's own options are appended as the Display tab.
LAYOUT = [
    ("settings_gambatte_tab_system", [
        ("settings_gambatte_section_system", ["gambatte_gb_hwmode", "gambatte_gb_bootloader"]),
    ]),
    ("settings_gambatte_tab_video", [
        ("settings_gambatte_section_colorization", [
            "gambatte_gb_colorization", "gambatte_gb_internal_palette",
            "gambatte_gb_palette_twb64_1", "gambatte_gb_palette_twb64_2",
            "gambatte_gb_palette_twb64_3", "gambatte_gb_palette_pixelshift_1",
        ]),
        ("settings_gambatte_section_color_correction", [
            "gambatte_gbc_color_correction", "gambatte_gbc_color_correction_mode",
            "gambatte_gbc_frontlight_position",
        ]),
        ("settings_gambatte_section_display", ["gambatte_dark_filter_level", "gambatte_mix_frames"]),
    ]),
    ("settings_gambatte_tab_audio", [
        ("settings_gambatte_section_audio", ["gambatte_audio_resampler"]),
    ]),
    ("settings_gambatte_tab_input", [
        ("settings_gambatte_section_input", [
            "gambatte_up_down_allowed", "gambatte_turbo_period", "gambatte_rumble_level",
        ]),
    ]),
]

# Listed only while another option has a value, like the core's own menus.
DEPENDS_ON = {
    "gambatte_gb_internal_palette": ("gambatte_gb_colorization", "internal"),
    "gambatte_gb_palette_twb64_1": ("gambatte_gb_internal_palette", "TWB64 - Pack 1"),
    "gambatte_gb_palette_twb64_2": ("gambatte_gb_internal_palette", "TWB64 - Pack 2"),
    "gambatte_gb_palette_twb64_3": ("gambatte_gb_internal_palette", "TWB64 - Pack 3"),
    "gambatte_gb_palette_pixelshift_1": ("gambatte_gb_internal_palette", "PixelShift - Pack 1"),
}

POSITIONS = [("hidden", "Hidden"), ("top_left", "Top left"), ("top_right", "Top right"),
             ("bottom_left", "Bottom left"), ("bottom_right", "Bottom right")]

# The overlay's own options: how the game is scaled, and the HUD. Shaders are
# picked in game (Settings > Shaders): tico cannot see the presets on the SD card.
OVERLAY_TAB = ("settings_gambatte_tab_display", [
    ("settings_gambatte_section_screen", [
        {"key": "display_mode", "label": "settings_gambatte_display_mode", "type": "enum",
         "default": "Integer", "choices": [("Integer", "Integer"), ("Display", "Display")]},
        {"key": "display_size", "label": "settings_gambatte_display_size", "type": "enum",
         "default": "Auto", "choices": [("Stretch", "Stretch"), ("4:3", "4:3"), ("16:9", "16:9"),
                                         ("Original", "Original"), ("1x", "1x"), ("2x", "2x"),
                                         ("Auto", "Auto")]},
    ]),
    ("settings_gambatte_section_fast_forward", [
        {"key": "fast_forward_speed", "label": "settings_gambatte_fast_forward_speed", "type": "enum",
         "default": "200", "choices": [("150", "150%"), ("200", "200%"), ("300", "300%"),
                                        ("400", "400%"), ("unlimited", "Unlimited")]},
        {"key": "fast_forward_mode", "label": "settings_gambatte_fast_forward_mode", "type": "enum",
         "default": "hold", "choices": [("hold", "Hold"), ("toggle", "Toggle")]},
        {"key": "fast_forward_hotkey", "label": "settings_gambatte_fast_forward_hotkey", "type": "enum",
         "default": "ZR", "choices": [("ZR", "ZR"), ("ZL", "ZL"), ("R", "R"), ("L", "L"),
                                       ("StickR", "Right stick"), ("StickL", "Left stick"),
                                       ("None", "Disabled")]},
    ]),
    ("settings_gambatte_section_hud", [
        {"key": "fps_counter_position", "label": "settings_gambatte_fps_counter", "type": "enum",
         "default": "hidden", "choices": POSITIONS},
        {"key": "rendered_ir_position", "label": "settings_gambatte_rendered_resolution",
         "type": "enum", "default": "hidden", "choices": POSITIONS},
    ]),
])

# Labels the core does not define. key -> (en, de, es, fr, ja, pt, ru, zh)
LABELS = {
    "settings_gambatte_tab_display": ("Display", "Anzeige", "Pantalla", "Affichage", "表示", "Tela",
                                      "Экран", "显示"),
    "settings_gambatte_section_screen": ("Screen", "Bild", "Imagen", "Image", "画面", "Imagem",
                                         "Изображение", "画面"),
    "settings_gambatte_section_hud": ("On-screen info", "Bildschirmanzeige", "Información en pantalla",
                                      "Affichage à l'écran", "画面表示", "Informações na tela",
                                      "Экранная информация", "屏幕信息"),
    "settings_gambatte_display_mode": ("Display Mode", "Anzeigemodus", "Modo de pantalla", "Mode d'affichage",
                                       "表示モード", "Modo de exibição", "Режим отображения",
                                       "显示模式"),
    "settings_gambatte_display_size": ("Size", "Größe", "Tamaño", "Taille", "サイズ", "Tamanho",
                                       "Размер", "尺寸"),
    "settings_gambatte_fps_counter": ("FPS counter", "FPS-Zähler", "Contador de FPS", "Compteur de FPS",
                                      "FPSカウンター", "Contador de FPS", "Счётчик FPS",
                                      "帧率计数器"),
    "settings_gambatte_section_fast_forward": ("Fast Forward", "Vorspulen", "Avance rápido",
                                               "Avance rapide", "早送り", "Avanço rápido",
                                               "Перемотка", "快进"),
    "settings_gambatte_fast_forward_speed": ("Fast forward speed", "Vorspul-Geschwindigkeit",
                                             "Velocidad de avance rápido", "Vitesse d'avance rapide",
                                             "早送りの速度", "Velocidade do avanço rápido",
                                             "Скорость перемотки", "快进速度"),
    "settings_gambatte_fast_forward_mode": ("Fast forward mode", "Vorspul-Modus",
                                            "Modo de avance rápido", "Mode d'avance rapide",
                                            "早送りモード", "Modo do avanço rápido",
                                            "Режим перемотки", "快进模式"),
    "settings_gambatte_fast_forward_hotkey": ("Fast forward button", "Vorspul-Taste",
                                              "Botón de avance rápido", "Bouton d'avance rapide",
                                              "早送りボタン", "Botão do avanço rápido",
                                              "Кнопка перемотки", "快进按键"),
    "settings_gambatte_rendered_resolution": ("Rendered resolution", "Gerenderte Auflösung", "Resolución renderizada",
                                              "Résolution de rendu", "描画解像度",
                                              "Resolução renderizada", "Разрешение рендеринга",
                                              "渲染分辨率"),
}

# Choice labels the core does not translate. English -> (de, es, fr, ja, pt, ru, zh)
CHOICES = {
    "Disabled": ("Deaktiviert", "Desactivado", "Désactivé", "無効", "Desativado", "Выключено",
                 "禁用"),
    "Integer": ("Ganzzahlig", "Entero", "Entier", "整数倍", "Inteiro", "Целочисленный", "整数"),
    "Display": ("Anzeige", "Pantalla", "Écran", "画面", "Tela", "Экран", "屏幕"),
    "Stretch": ("Strecken", "Estirar", "Étirer", "引き伸ばし", "Esticar", "Растянуть", "拉伸"),
    "Original": ("Original", "Original", "Original", "オリジナル", "Original", "Оригинал", "原始"),
    "Auto": ("Auto", "Auto", "Auto", "自動", "Auto", "Авто", "自动"),
    "Unlimited": ("Unbegrenzt", "Ilimitado", "Illimité", "無制限", "Ilimitado", "Без ограничений",
                  "无限制"),
    "Hold": ("Halten", "Mantener", "Maintenir", "長押し", "Segurar", "Удерживать", "按住"),
    "Toggle": ("Umschalten", "Alternar", "Basculer", "切り替え", "Alternar", "Переключать", "切换"),
    "Right stick": ("Rechter Stick", "Stick derecho", "Stick droit", "右スティック", "Analógico direito",
                    "Правый стик", "右摇杆"),
    "Left stick": ("Linker Stick", "Stick izquierdo", "Stick gauche", "左スティック", "Analógico esquerdo",
                   "Левый стик", "左摇杆"),
    "Hidden": ("Ausgeblendet", "Oculto", "Masqué", "非表示", "Oculto", "Скрыто", "隐藏"),
    "Top left": ("Oben links", "Arriba a la izquierda", "En haut à gauche", "左上",
                 "Superior esquerdo", "Сверху слева", "左上"),
    "Top right": ("Oben rechts", "Arriba a la derecha", "En haut à droite", "右上",
                  "Superior direito", "Сверху справа", "右上"),
    "Bottom left": ("Unten links", "Abajo a la izquierda", "En bas à gauche", "左下",
                    "Inferior esquerdo", "Снизу слева", "左下"),
    "Bottom right": ("Unten rechts", "Abajo a la derecha", "En bas à droite", "右下",
                     "Inferior direito", "Снизу справа", "右下"),
}

RESTART_SUFFIX = re.compile(r"\s*\((Restart Required|[^)]*[Nn]eustart[^)]*|[^)]*[Rr]einici[^)]*|"
                            r"[^)]*[Rr]edémarr[^)]*|[^)]*再起動[^)]*|[^)]*перезапуск[^)]*|"
                            r"[^)]*重启[^)]*|[^)]*[Rr]einicializa[^)]*)\)")


def value_key(label: str) -> str:
    """tico_config.cpp's ValueKey: settings_gambatte_value_ + label as a slug."""
    return "settings_gambatte_value_" + "_".join(re.findall(r"[a-z0-9]+", label.lower()))


def clean_label(text: str) -> str:
    return RESTART_SUFFIX.sub("", text).lstrip("> ").strip()


def english_choice(value: str, label: str) -> str:
    # the core leaves on/off style values unlabelled
    return label.capitalize() if label == value and value in ("disabled", "enabled") else label


def dump_options() -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "dump_core_options"
        subprocess.run(["cc", "-std=gnu11", "-w",
                        "-I", str(ROOT / "libgambatte/libretro"),
                        "-I", str(ROOT / "libgambatte/libretro-common/include"),
                        "-o", str(exe), str(Path(__file__).with_name("dump_core_options.c"))],
                       check=True)
        return json.loads(subprocess.run([str(exe)], check=True, capture_output=True,
                                         text=True).stdout)


def label_key(key: str) -> str:
    return LABEL_KEYS.get(key, "settings_gambatte_" + key.removeprefix("gambatte_"))


def build_settings(dump: dict) -> dict:
    core = {o["key"]: o for o in dump["en"]}
    placed = {key for _, sections in LAYOUT for _, keys in sections for key in keys}
    missing = sorted(set(core) - placed)
    if missing:
        raise SystemExit(f"core options not placed in LAYOUT: {missing}")

    tabs = []
    for tab, sections in LAYOUT:
        out_sections = []
        for title, keys in sections:
            options = []
            for key in keys:
                source = core[key]
                values = [v for v, _ in source["values"]]
                option = {"key": key, "label": label_key(key)}
                if sorted(values) == ["disabled", "enabled"]:
                    option.update(type="bool", default=source["default"])
                else:
                    option.update(type="enum", default=source["default"], choices=[
                        {"label": english_choice(v, l), "value": v} for v, l in source["values"]])
                if RESTART_SUFFIX.search(source["desc"]):
                    option["restart"] = True
                if key in DEPENDS_ON:
                    on, value = DEPENDS_ON[key]
                    option["depends_on"] = {"key": on, "value": value}
                options.append(option)
            out_sections.append({"title": title, "options": options})
        tabs.append({"name": tab, "sections": out_sections})

    tab, sections = OVERLAY_TAB
    tabs.insert(1, {"name": tab, "sections": [
        {"title": title, "options": [
            {**o, "choices": [{"label": l, "value": v} for v, l in o["choices"]]}
            for o in options]}
        for title, options in sections]})

    return {
        "core_id": "gambatte",
        "display_name": "Gambatte",
        "config_file": "gambatte.jsonc",
        "slugs": ["gb", "gbc"],
        "bool_true_value": "enabled",
        "bool_false_value": "disabled",
        "tabs": tabs,
    }


def tico_owned(key: str) -> bool:
    """Labels tico itself defines, whose wording tico keeps."""
    return key in LABEL_KEYS.values() or key.startswith(("settings_gambatte_tab_",
                                                          "settings_gambatte_section_"))


def build_strings(dump: dict, settings: dict,
                  existing: dict[str, dict[str, str]]) -> dict[str, dict[str, str]]:
    strings: dict[str, dict[str, str]] = {lang: {} for lang in LANGUAGES}
    english = {o["key"]: o for o in dump["en"]}
    for lang in LANGUAGES:
        out = strings[lang]
        index = LANGUAGES.index(lang)
        for option in dump[lang]:
            key, en = option["key"], english[option["key"]]
            out[label_key(key)] = clean_label(option["desc"])
            for (value, label), (_, en_label) in zip(option["values"], en["values"]):
                en_text = english_choice(value, en_label)
                if label != en_label:
                    out[value_key(en_text)] = label
        for key, texts in LABELS.items():
            out[key] = texts[index]
        if lang != "en":
            for choice, translations in CHOICES.items():
                out[value_key(choice)] = translations[index - 1]
        if lang == "en":
            for key in list(out):
                if key.startswith("settings_gambatte_value_"):
                    del out[key]
    # tico's own labels keep tico's wording: from tico-nx when it is checked
    # out, otherwise as the language files already have them
    for lang in LANGUAGES:
        path = TICO_NX / "assets/lang" / f"{lang}.json"
        source = json.loads(path.read_text()) if path.exists() else existing[lang]
        for key, value in source.items():
            if tico_owned(key):
                strings[lang][key] = value
    used = set()

    def walk(node):
        if isinstance(node, dict):
            for field in ("label", "name", "title"):
                if isinstance(node.get(field), str) and node[field].startswith("settings_"):
                    used.add(node[field])
            for child in node.values():
                walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)

    walk(settings)
    unlabelled = sorted(used - set(strings["en"]))
    if unlabelled:
        raise SystemExit(f"labels without English text: {unlabelled}")
    return strings


def main() -> None:
    dump = dump_options()
    settings = build_settings(dump)
    SETTINGS.write_text(json.dumps(settings, indent=2, ensure_ascii=False) + "\n")
    current_files = {lang: json.loads((LANG_DIR / f"{lang}.json").read_text())
                     if (LANG_DIR / f"{lang}.json").exists() else {} for lang in LANGUAGES}
    for lang, strings in build_strings(dump, settings, current_files).items():
        path = LANG_DIR / f"{lang}.json"
        current = {k: v for k, v in current_files[lang].items()
                   if not k.startswith("settings_gambatte_")}
        current.update(dict(sorted(strings.items())))
        path.write_text(json.dumps(current, indent=4, ensure_ascii=False) + "\n")
    print(f"wrote {SETTINGS.relative_to(ROOT)} and {len(LANGUAGES)} language files")


if __name__ == "__main__":
    main()
