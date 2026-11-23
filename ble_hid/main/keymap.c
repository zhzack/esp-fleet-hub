#include "keymap.h"

#include <string.h>
#include <ctype.h>

/* HID modifier bits */
#define MOD_LCTRL  0x01
#define MOD_LSHIFT 0x02
#define MOD_LALT   0x04
#define MOD_LGUI   0x08

static const struct {
    const char *name;
    uint8_t     code;
    uint8_t     mods;
} TABLE[] = {
    /* Letters */
    {"KEY_A", 0x04, 0}, {"KEY_B", 0x05, 0}, {"KEY_C", 0x06, 0}, {"KEY_D", 0x07, 0},
    {"KEY_E", 0x08, 0}, {"KEY_F", 0x09, 0}, {"KEY_G", 0x0A, 0}, {"KEY_H", 0x0B, 0},
    {"KEY_I", 0x0C, 0}, {"KEY_J", 0x0D, 0}, {"KEY_K", 0x0E, 0}, {"KEY_L", 0x0F, 0},
    {"KEY_M", 0x10, 0}, {"KEY_N", 0x11, 0}, {"KEY_O", 0x12, 0}, {"KEY_P", 0x13, 0},
    {"KEY_Q", 0x14, 0}, {"KEY_R", 0x15, 0}, {"KEY_S", 0x16, 0}, {"KEY_T", 0x17, 0},
    {"KEY_U", 0x18, 0}, {"KEY_V", 0x19, 0}, {"KEY_W", 0x1A, 0}, {"KEY_X", 0x1B, 0},
    {"KEY_Y", 0x1C, 0}, {"KEY_Z", 0x1D, 0},
    /* Digits */
    {"KEY_1", 0x1E, 0}, {"KEY_2", 0x1F, 0}, {"KEY_3", 0x20, 0}, {"KEY_4", 0x21, 0},
    {"KEY_5", 0x22, 0}, {"KEY_6", 0x23, 0}, {"KEY_7", 0x24, 0}, {"KEY_8", 0x25, 0},
    {"KEY_9", 0x26, 0}, {"KEY_0", 0x27, 0},
    /* Specials */
    {"KEY_ENTER",     0x28, 0}, {"KEY_ESC",       0x29, 0},
    {"KEY_BACKSPACE", 0x2A, 0}, {"KEY_TAB",       0x2B, 0},
    {"KEY_SPACE",     0x2C, 0}, {"KEY_MINUS",     0x2D, 0},
    {"KEY_EQUAL",     0x2E, 0},
    /* Arrows */
    {"KEY_RIGHT", 0x4F, 0}, {"KEY_LEFT", 0x50, 0},
    {"KEY_DOWN",  0x51, 0}, {"KEY_UP",   0x52, 0},
    /* Function */
    {"KEY_F1", 0x3A, 0}, {"KEY_F2", 0x3B, 0}, {"KEY_F3", 0x3C, 0}, {"KEY_F4", 0x3D, 0},
    {"KEY_F5", 0x3E, 0}, {"KEY_F6", 0x3F, 0}, {"KEY_F7", 0x40, 0}, {"KEY_F8", 0x41, 0},
    {"KEY_F9", 0x42, 0}, {"KEY_F10",0x43, 0}, {"KEY_F11",0x44, 0}, {"KEY_F12",0x45, 0},
    /* Punctuation (US layout) */
    {"KEY_LBRACKET",  0x2F, 0}, {"KEY_RBRACKET", 0x30, 0}, {"KEY_BACKSLASH", 0x31, 0},
    {"KEY_SEMICOLON", 0x33, 0}, {"KEY_QUOTE",    0x34, 0}, {"KEY_BACKQUOTE", 0x35, 0},
    {"KEY_COMMA",     0x36, 0}, {"KEY_DOT",      0x37, 0}, {"KEY_SLASH",     0x38, 0},
    /* Editing keys */
    {"KEY_CAPSLOCK", 0x39, 0},
    {"KEY_INSERT",   0x49, 0}, {"KEY_HOME",     0x4A, 0}, {"KEY_PAGEUP",   0x4B, 0},
    {"KEY_DELETE",   0x4C, 0}, {"KEY_END",      0x4D, 0}, {"KEY_PAGEDOWN", 0x4E, 0},
    /* Modifiers — code 0 with the modifier bit set (pure modifier report).
     * Right-side variants map onto the left bits; hosts rarely distinguish. */
    {"KEY_LEFTCTRL",   0x00, MOD_LCTRL},  {"KEY_LEFTSHIFT", 0x00, MOD_LSHIFT},
    {"KEY_LEFTALT",    0x00, MOD_LALT},   {"KEY_LEFTGUI",   0x00, MOD_LGUI},
    {"KEY_RIGHTCTRL",  0x00, MOD_LCTRL},  {"KEY_RIGHTSHIFT",0x00, MOD_LSHIFT},
    {"KEY_RIGHTALT",   0x00, MOD_LALT},   {"KEY_RIGHTGUI",  0x00, MOD_LGUI},
    /* Combos (common) */
    {"KEY_CTRL_C",     0x06, MOD_LCTRL},
    {"KEY_CTRL_V",     0x19, MOD_LCTRL},
    {"KEY_CTRL_X",     0x1B, MOD_LCTRL},
    {"KEY_CTRL_Z",     0x1D, MOD_LCTRL},
    {"KEY_CTRL_A",     0x04, MOD_LCTRL},
    {"KEY_CTRL_S",     0x16, MOD_LCTRL},
    {"KEY_ALT_TAB",    0x2B, MOD_LALT},
    {"KEY_ALT_SHIFT_TAB", 0x2B, MOD_LALT | MOD_LSHIFT},
    {"KEY_GUI_L",      0x0F, MOD_LGUI},
    {"KEY_GUI_D",      0x07, MOD_LGUI},
    {"KEY_GUI_TAB",    0x2B, MOD_LGUI},
    {"KEY_GUI_CTRL_LEFT",  0x50, MOD_LGUI | MOD_LCTRL},
    {"KEY_GUI_CTRL_RIGHT", 0x4F, MOD_LGUI | MOD_LCTRL},
};

bool keymap_lookup(const char *name, uint8_t *out_code, uint8_t *out_mods)
{
    if (!name) return false;
    for (size_t i = 0; i < sizeof(TABLE)/sizeof(TABLE[0]); ++i) {
        if (strcmp(name, TABLE[i].name) == 0) {
            *out_code = TABLE[i].code;
            *out_mods = TABLE[i].mods;
            return true;
        }
    }
    return false;
}

bool keymap_ascii(char c, uint8_t *out_code, uint8_t *out_mods)
{
    *out_mods = 0;
    if (c >= 'a' && c <= 'z') { *out_code = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *out_code = 0x04 + (c - 'A'); *out_mods = MOD_LSHIFT; return true; }
    if (c >= '1' && c <= '9') { *out_code = 0x1E + (c - '1'); return true; }
    if (c == '0') { *out_code = 0x27; return true; }
    switch (c) {
    case ' ':  *out_code = 0x2C; return true;
    case '\n': *out_code = 0x28; return true;
    case '\t': *out_code = 0x2B; return true;
    case '-':  *out_code = 0x2D; return true;
    case '=':  *out_code = 0x2E; return true;
    case '[':  *out_code = 0x2F; return true;
    case ']':  *out_code = 0x30; return true;
    case '\\': *out_code = 0x31; return true;
    case ';':  *out_code = 0x33; return true;
    case '\'': *out_code = 0x34; return true;
    case '`':  *out_code = 0x35; return true;
    case ',':  *out_code = 0x36; return true;
    case '.':  *out_code = 0x37; return true;
    case '/':  *out_code = 0x38; return true;
    /* shifted */
    case '!':  *out_code = 0x1E; *out_mods = MOD_LSHIFT; return true;
    case '@':  *out_code = 0x1F; *out_mods = MOD_LSHIFT; return true;
    case '#':  *out_code = 0x20; *out_mods = MOD_LSHIFT; return true;
    case '$':  *out_code = 0x21; *out_mods = MOD_LSHIFT; return true;
    case '%':  *out_code = 0x22; *out_mods = MOD_LSHIFT; return true;
    case '^':  *out_code = 0x23; *out_mods = MOD_LSHIFT; return true;
    case '&':  *out_code = 0x24; *out_mods = MOD_LSHIFT; return true;
    case '*':  *out_code = 0x25; *out_mods = MOD_LSHIFT; return true;
    case '(':  *out_code = 0x26; *out_mods = MOD_LSHIFT; return true;
    case ')':  *out_code = 0x27; *out_mods = MOD_LSHIFT; return true;
    case '_':  *out_code = 0x2D; *out_mods = MOD_LSHIFT; return true;
    case '+':  *out_code = 0x2E; *out_mods = MOD_LSHIFT; return true;
    case '{':  *out_code = 0x2F; *out_mods = MOD_LSHIFT; return true;
    case '}':  *out_code = 0x30; *out_mods = MOD_LSHIFT; return true;
    case '|':  *out_code = 0x31; *out_mods = MOD_LSHIFT; return true;
    case ':':  *out_code = 0x33; *out_mods = MOD_LSHIFT; return true;
    case '"': *out_code = 0x34; *out_mods = MOD_LSHIFT; return true;
    case '~':  *out_code = 0x35; *out_mods = MOD_LSHIFT; return true;
    case '<':  *out_code = 0x36; *out_mods = MOD_LSHIFT; return true;
    case '>':  *out_code = 0x37; *out_mods = MOD_LSHIFT; return true;
    case '?':  *out_code = 0x38; *out_mods = MOD_LSHIFT; return true;
    }
    return false;
}

static const struct {
    const char *name;
    uint16_t    code;
} CONSUMER_TABLE[] = {
    {"KEY_MUTE",        0x00E2},
    {"KEY_MEDIA_MUTE",  0x00E2},
    {"KEY_VOL_UP",      0x00E9},
    {"KEY_VOLUME_UP",   0x00E9},
    {"KEY_VOL_DOWN",    0x00EA},
    {"KEY_VOLUME_DOWN", 0x00EA},
    {"KEY_PLAY_PAUSE",  0x00CD},
    {"KEY_NEXT_TRACK",  0x00B5},
    {"KEY_PREV_TRACK",  0x00B6},
    {"KEY_POWER",       0x0030},
    {"KEY_SLEEP",       0x0032},
};

bool keymap_consumer_lookup(const char *name, uint16_t *out_code)
{
    if (!name || !out_code) return false;
    for (size_t i = 0; i < sizeof(CONSUMER_TABLE)/sizeof(CONSUMER_TABLE[0]); ++i) {
        if (strcmp(name, CONSUMER_TABLE[i].name) == 0) {
            *out_code = CONSUMER_TABLE[i].code;
            return true;
        }
    }
    return false;
}
