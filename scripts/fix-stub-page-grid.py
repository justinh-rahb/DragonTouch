#!/usr/bin/env python3
from pathlib import Path
import shutil
import sys
import time

ROOT = Path.cwd().resolve()
P = ROOT / "components/dt_ui/dt_ui.c"

if not P.exists():
    sys.exit(f"ERROR: missing {P}")

src = P.read_text()

if "DT_UI_STUB_GRID_FIX" in src:
    print("Stub grid fix already installed.")
    sys.exit(0)

def find_function(text, signature):
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f"function missing: {signature}")

    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"opening brace missing: {signature}")

    depth = 0
    i = brace
    in_string = in_char = in_line = in_block = escape = False

    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""

        if in_line:
            if c == "\n":
                in_line = False
            i += 1
            continue

        if in_block:
            if c == "*" and n == "/":
                in_block = False
                i += 2
                continue
            i += 1
            continue

        if in_string:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == '"':
                in_string = False
            i += 1
            continue

        if in_char:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == "'":
                in_char = False
            i += 1
            continue

        if c == "/" and n == "/":
            in_line = True
            i += 2
            continue

        if c == "/" and n == "*":
            in_block = True
            i += 2
            continue

        if c == '"':
            in_string = True
            i += 1
            continue

        if c == "'":
            in_char = True
            i += 1
            continue

        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return start, i + 1

        i += 1

    raise RuntimeError(f"closing brace missing: {signature}")

def replace_function(text, signature, replacement):
    a, b = find_function(text, signature)
    return text[:a] + replacement + text[b:]

required = [
    "static void create_stub_page(",
    "LV_FLEX_FLOW_ROW_WRAP",
    "lv_obj_set_size(card, 220, 116)",
    "lv_obj_set_flex_grow(card, 1)",
]

for marker in required:
    if marker not in src:
        sys.exit(
            "ERROR: expected old stub-layout marker missing:\n"
            f"  {marker}\n"
            "Nothing modified."
        )

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = (
    ROOT
    / "backups"
    / f"pre-stub-grid-fix-{stamp}"
    / P.relative_to(ROOT)
)

backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(P, backup)
print(f"Backup: {backup}")

replacement = '''static void create_stub_page(
    lv_obj_t *page,
    const char *title,
    const char *description,
    const char *const *cards,
    size_t card_count
)
{
    /*
     * DT_UI_STUB_GRID_FIX
     *
     * Avoid wrapping flex rows with fixed-width children that also
     * flex-grow. Settings is the first stub with four cards and is
     * therefore the first page that crosses onto a second flex line.
     *
     * A two-column grid has deterministic geometry and avoids that
     * flex-wrap path entirely.
     */
    static int32_t col_dsc[] = {
        LV_GRID_FR(1),
        LV_GRID_FR(1),
        LV_GRID_TEMPLATE_LAST
    };

    static int32_t row_dsc[] = {
        116,
        116,
        116,
        116,
        LV_GRID_TEMPLATE_LAST
    };

    lv_obj_set_layout(
        page,
        LV_LAYOUT_FLEX
    );

    lv_obj_set_flex_flow(
        page,
        LV_FLEX_FLOW_COLUMN
    );

    lv_obj_t *heading =
        make_label(
            page,
            title,
            DT_COLOR_TEXT
        );

    lv_obj_set_style_text_font(
        heading,
        &lv_font_montserrat_20,
        0
    );

    lv_obj_t *body =
        make_label(
            page,
            description,
            DT_COLOR_MUTED
        );

    lv_label_set_long_mode(
        body,
        LV_LABEL_LONG_MODE_WRAP
    );

    lv_obj_set_width(
        body,
        LV_PCT(100)
    );

    lv_obj_t *grid =
        lv_obj_create(page);

    lv_obj_remove_style_all(grid);

    lv_obj_set_width(
        grid,
        LV_PCT(100)
    );

    lv_obj_set_flex_grow(
        grid,
        1
    );

    lv_obj_set_layout(
        grid,
        LV_LAYOUT_GRID
    );

    lv_obj_set_grid_dsc_array(
        grid,
        col_dsc,
        row_dsc
    );

    lv_obj_set_style_pad_row(
        grid,
        10,
        0
    );

    lv_obj_set_style_pad_column(
        grid,
        10,
        0
    );

    for (
        size_t i = 0;
        i < card_count;
        ++i
    ) {
        const int32_t col =
            (int32_t)(i % 2);

        const int32_t row =
            (int32_t)(i / 2);

        if (row >= 4) {
            ESP_LOGW(
                TAG,
                "stub page '%s' has too many cards: %u",
                title,
                (unsigned)card_count
            );
            break;
        }

        lv_obj_t *card =
            make_card(
                grid,
                cards[i]
            );

        lv_obj_set_grid_cell(
            card,
            LV_GRID_ALIGN_STRETCH,
            col,
            1,
            LV_GRID_ALIGN_STRETCH,
            row,
            1
        );

        lv_obj_t *availability =
            make_label(
                card,
                "Available after printer pairing",
                DT_COLOR_MUTED
            );

        lv_label_set_long_mode(
            availability,
            LV_LABEL_LONG_MODE_WRAP
        );

        lv_obj_set_width(
            availability,
            LV_PCT(100)
        );

        lv_obj_t *button =
            make_action(
                card,
                "Unavailable",
                false
            );

        lv_obj_set_width(
            button,
            LV_PCT(100)
        );

        lv_obj_set_flex_grow(
            button,
            0
        );

        set_button_enabled(
            button,
            false
        );
    }
}'''

src = replace_function(
    src,
    "static void create_stub_page(",
    replacement
)

P.write_text(src)

print(f"Patched: {P}")
print()
print("Changed:")
print("  - stub cards: wrapping flex -> deterministic 2-column grid")
print("  - removed stub-card flex growth")
print("  - stub buttons no longer flex-grow")
print()
print("Unchanged:")
print("  - lazy page construction")
print("  - display / RGB buffering")
print("  - GT911")
print("  - Stage 2 runtime")
print("  - watchdog configuration")
