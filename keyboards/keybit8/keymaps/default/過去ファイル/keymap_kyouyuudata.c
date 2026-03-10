#include QMK_KEYBOARD_H
#include "quantum.h"
#include "timer.h"
#include <stdlib.h>

// --- 定義とピン設定 ---
enum layer_names { _BASE, _LAYER1, _LAYER2, _LAYER3 };
enum SystemMode { MODE_INPUT, MODE_GAME };
enum GameMode { GAME_NONE, GAME_HAYAOSHI, GAME_RUSSIAN, GAME_SLOT, GAME_SIMON, GAME_ORGAN, GAME_MEOSHI, GAME_MOGURA };

static uint8_t current_mode = MODE_GAME; 
static uint8_t current_game = GAME_NONE;
static bool lock_game_start = false; 

static const pin_t red_led_pins[] = { D3, B4, B3, F7, D2, B2, B1, F6 };
#define GREEN_LED_PIN B5
#define SPEAKER_PIN B6

// --- ゲーム用共通変数 ---
static int combo = 0;
static uint32_t game_timer = 0;
static uint8_t simon_seq[32];
static bool game_active = false;

// --- 共通ハードウェア制御 ---
void clear_all_leds(void) {
    for (int i = 0; i < 8; i++) writePinLow(red_led_pins[i]);
    writePinLow(GREEN_LED_PIN);
}

void play_tone(uint16_t freq, uint16_t duration) {
    if (freq == 0) { wait_ms(duration); return; }
    uint32_t period = 1000000 / freq;
    uint32_t elapsed = 0;
    uint32_t duration_us = (uint32_t)duration * 1000;
    while (elapsed < duration_us) {
        writePinHigh(SPEAKER_PIN);
        wait_us(period / 10); 
        writePinLow(SPEAKER_PIN);
        wait_us(period * 9 / 10);
        elapsed += period;
    }
}

void play_start_melody(void) {
    uint16_t notes[] = {523, 659, 784, 1047};
    for (int i = 0; i < 4; i++) {
        play_tone(notes[i], 120);
        wait_ms(20);
    }
}

// --- 各ゲームロジック ---

void run_hayaoshi(void) {
    static bool target_on = false;
    static int target_idx = 0;
    if (!target_on) {
        wait_ms(500 + (rand() % 1500));
        target_idx = rand() % 8;
        writePinHigh(red_led_pins[target_idx]);
        target_on = true;
    }
    for (int i = 0; i < 8; i++) {
        if (matrix_is_on(i / 4, i % 4)) {
            if (i == target_idx && target_on) {
                play_tone(1000 + (combo * 50), 100); combo++;
            } else {
                play_tone(180, 300); combo = 0;
            }
            writePinLow(red_led_pins[target_idx]);
            target_on = false; wait_ms(500);
        }
    }
}

void run_russian(void) {
    static int bullet = -1;
    if (bullet == -1) bullet = rand() % 8;
    for (int i = 0; i < 8; i++) {
        if (matrix_is_on(i / 4, i % 4)) {
            if (i == bullet) {
                for(int j=0; j<3; j++){ play_tone(120, 100); play_tone(80, 100); }
                bullet = -1; clear_all_leds(); wait_ms(500);
            } else {
                writePinHigh(red_led_pins[i]);
                play_tone(880, 60); wait_ms(200);
            }
        }
    }
}

void run_slot(void) {
    static int pos = 0;
    // MAXマクロを使用
    int speed = MAX(40, 200 - (combo * 15));
    if (timer_elapsed32(game_timer) > speed) {
        writePinLow(red_led_pins[pos]);
        pos = (pos + 1) % 8;
        writePinHigh(red_led_pins[pos]);
        game_timer = timer_read32();
    }
    if (matrix_is_on(pos / 4, pos % 4)) {
        play_tone(1500 + (combo * 20), 50); combo++; wait_ms(250);
    }
}

void run_simon(void) {
    static int step = 0;
    if (!game_active) {
        if(combo >= 32) combo = 0;
        simon_seq[combo] = rand() % 8; combo++;
        wait_ms(500);
        for(int i=0; i<combo; i++) {
            writePinHigh(red_led_pins[simon_seq[i]]);
            play_tone(440 + (simon_seq[i]*60), 250);
            writePinLow(red_led_pins[simon_seq[i]]);
            wait_ms(150);
        }
        step = 0; game_active = true;
    } else {
        for(int i=0; i<8; i++) {
            if(matrix_is_on(i/4, i%4)) {
                if(i == simon_seq[step]) {
                    play_tone(440+(i*60), 200); step++;
                    if(step >= combo) { wait_ms(400); play_tone(1200, 100); game_active = false; }
                } else {
                    play_tone(150, 500); combo = 1; game_active = false;
                }
                wait_ms(300);
            }
        }
    }
}

void run_organ(void) {
    uint16_t freqs[] = {262, 294, 330, 349, 392, 440, 494, 523};
    bool any = false;
    for(int i=0; i<8; i++) {
        if(matrix_is_on(i/4, i%4)) {
            writePinHigh(red_led_pins[i]);
            play_tone(freqs[i], 5); any = true;
        } else { writePinLow(red_led_pins[i]); }
    }
    if(!any) wait_ms(1);
}

void run_meoshi(void) {
    static int pos = 0;
    if (timer_elapsed32(game_timer) > 120) {
        writePinLow(red_led_pins[pos]);
        pos = (pos + 1) % 8;
        writePinHigh(red_led_pins[pos]);
        game_timer = timer_read32();
    }
    if (matrix_is_on(1, 2)) { 
        if (pos == 6) { 
            for(int i=0;i<3;i++){ play_tone(1500,80); play_tone(2000,80); }
        } else { play_tone(150, 400); }
        wait_ms(600);
    }
}

void run_mogura(void) {
    static int target = -1;
    if (timer_read32() > game_timer) {
        if (target != -1) writePinLow(red_led_pins[target]);
        target = rand() % 8;
        writePinHigh(red_led_pins[target]);
        // MAXマクロを使用
        game_timer = timer_read32() + MAX(250, 900 - (combo * 40));
    }
    for(int i=0; i<8; i++) {
        if(matrix_is_on(i/4, i%4) && i == target) {
            play_tone(1200, 40); combo++; target = -1;
            game_timer = timer_read32() + 150;
        }
    }
}

// --- メイン制御ループ ---

void matrix_scan_user(void) {
    static uint32_t mode_timer = 0;
    bool mode_keys = (matrix_is_on(0,0) && matrix_is_on(0,1) && matrix_is_on(1,0) && matrix_is_on(1,1));

    if (mode_keys) {
        if (mode_timer == 0) mode_timer = timer_read32();
        if (timer_elapsed32(mode_timer) > 2000) {
            current_mode = (current_mode == MODE_GAME) ? MODE_INPUT : MODE_GAME;
            current_game = GAME_NONE; clear_all_leds(); play_start_melody();
            if (current_mode == MODE_GAME) lock_game_start = true; 
            mode_timer = 0; wait_ms(1000);
        }
    } else {
        mode_timer = 0;
        if (lock_game_start && !matrix_is_on(0,0) && !matrix_is_on(0,1) && !matrix_is_on(1,0) && !matrix_is_on(1,1)) {
            lock_game_start = false; 
        }
    }

    if (current_mode == MODE_INPUT) return;

    if (current_game != GAME_NONE) {
        if (matrix_is_on(0,2) && matrix_is_on(0,3) && matrix_is_on(1,2) && matrix_is_on(1,3)) {
            current_game = GAME_NONE; clear_all_leds();
            play_tone(200, 200); wait_ms(500); return;
        }
    }

    if (current_game == GAME_NONE && !lock_game_start) {
        if (matrix_is_on(0,0)) {
            if (matrix_is_on(0,1))      current_game = GAME_HAYAOSHI;
            else if (matrix_is_on(0,2)) current_game = GAME_RUSSIAN;
            else if (matrix_is_on(0,3)) current_game = GAME_SLOT;
            else if (matrix_is_on(1,0)) current_game = GAME_SIMON;
            else if (matrix_is_on(1,1)) current_game = GAME_ORGAN;
            else if (matrix_is_on(1,2)) current_game = GAME_MEOSHI;
            else if (matrix_is_on(1,3)) current_game = GAME_MOGURA;

            if (current_game != GAME_NONE) {
                combo = 0; game_active = false;
                clear_all_leds(); play_start_melody();
            }
        }
        for (int i=0; i<8; i++) writePin(red_led_pins[i], matrix_is_on(i/4, i%4));
    } else if (current_game != GAME_NONE) {
        switch (current_game) {
            case GAME_HAYAOSHI: run_hayaoshi(); break;
            case GAME_RUSSIAN:  run_russian();  break;
            case GAME_SLOT:     run_slot();     break;
            case GAME_SIMON:    run_simon();    break;
            case GAME_ORGAN:    run_organ();    break;
            case GAME_MEOSHI:   run_meoshi();   break;
            case GAME_MOGURA:   run_mogura();   break;
        }
    }
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (current_mode == MODE_GAME) return false;
    uint8_t index = record->event.key.row * 4 + record->event.key.col;
    if (index < 8) {
        if (record->event.pressed) writePinHigh(red_led_pins[index]);
        else writePinLow(red_led_pins[index]);
    }
    return true;
}

layer_state_t layer_state_set_user(layer_state_t state) {
    if (current_mode == MODE_INPUT) writePin(GREEN_LED_PIN, get_highest_layer(state) > 0);
    return state;
}

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [_BASE] = LAYOUT(KC_W, KC_A, KC_S, LT(_LAYER3, KC_D), KC_SPC, KC_LSFT, LT(_LAYER2, KC_E), LT(_LAYER1, KC_1)),
    [_LAYER1] = LAYOUT(KC_2, KC_3, KC_4, KC_5, KC_6, KC_7, KC_8, KC_9),
    [_LAYER2] = LAYOUT(KC_Q, KC_R, KC_F, KC_TAB, KC_ENT, KC_ESC, KC_T, KC_C),
    [_LAYER3] = LAYOUT(KC_LEFT, KC_UP, KC_DOWN, KC_RIGHT, KC_0, KC_1, KC_2, KC_SPC)
};

void keyboard_post_init_user(void) {
    for (int i = 0; i < 8; i++) setPinOutput(red_led_pins[i]);
    setPinOutput(GREEN_LED_PIN); setPinOutput(SPEAKER_PIN);
}