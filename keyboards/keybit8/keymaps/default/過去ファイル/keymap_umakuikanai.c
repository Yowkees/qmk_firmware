#include QMK_KEYBOARD_H

enum layer_names {
    _BASE,
    _FN
};

typedef enum {
    MODE_NORMAL,
    MODE_ROULETTE,
    MODE_HAYAOshi
} game_mode_t;

static game_mode_t game_mode = MODE_NORMAL;
static uint16_t target_key = KC_NO;


// ----------------------
// キーマップ（VIA対応）
// ----------------------

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {

    [_BASE] = LAYOUT(
        KC_1, KC_2, KC_3, KC_4,
        KC_5, KC_6, KC_7, KC_8
    ),

    [_FN] = LAYOUT(
        KC_F1, KC_F2, _______, _______,
        _______, _______, _______, _______
    )
};


#ifdef AUDIO_ENABLE

// ✅ 正しいQMK形式
#define TONE_C   SONG(SINGLE_NOTE(NOTE_C4))
#define TONE_D   SONG(SINGLE_NOTE(NOTE_D4))
#define TONE_E   SONG(SINGLE_NOTE(NOTE_E4))
#define TONE_F   SONG(SINGLE_NOTE(NOTE_F4))
#define TONE_G   SONG(SINGLE_NOTE(NOTE_G4))
#define TONE_A   SONG(SINGLE_NOTE(NOTE_A4))
#define TONE_B   SONG(SINGLE_NOTE(NOTE_B4))
#define TONE_C5  SONG(SINGLE_NOTE(NOTE_C5))

#define SUCCESS  SONG(NOTE_C5, NOTE_E5)
#define FAIL     SONG(SINGLE_NOTE(NOTE_C4))

#endif


bool process_record_user(uint16_t keycode, keyrecord_t *record) {

#ifdef AUDIO_ENABLE
    if (record->event.pressed) {

        // 🎮 F1 → ルーレット
        if (keycode == KC_F1) {
            game_mode = MODE_ROULETTE;
            return false;
        }

        // ⚡ F2 → 早押し
        if (keycode == KC_F2) {
            game_mode = MODE_HAYAOshi;
            target_key = KC_1 + (rand() % 8);
            PLAY_SONG(TONE_C5);
            return false;
        }

        // 🎲 ルーレット
        if (game_mode == MODE_ROULETTE) {
