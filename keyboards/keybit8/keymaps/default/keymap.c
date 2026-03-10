/*
 * Keybit8 QMK Firmware - keymap.c  v4
 * ゲーム統合版：オルガン / 目押しルーレット / 早押しゲーム
 *
 * ゲーム起動コンビ：キー0+1=オルガン / 0+2=目押しルーレット / 0+3=早押しゲーム
 * モード切り替え：0+1+4+5を2秒長押し（ゲーム⇔入力）
 * ゲーム終了：2+3+6+7同時押し
 */

#include QMK_KEYBOARD_H
#include <stdlib.h>

#ifdef AUDIO_ENABLE
#include "audio.h"
#endif

// ============================================================
// レイヤー定義
// ============================================================
enum layer_names { _BASE, _LAYER1, _LAYER2, _LAYER3 };

// ============================================================
// モード・ゲーム定義
// ============================================================
typedef enum { MODE_GAME, MODE_INPUT } SystemMode;
typedef enum { GAME_NONE, GAME_ORGAN, GAME_MEOSHI, GAME_MOGURA } GameMode;

static SystemMode current_mode      = MODE_INPUT;  // 起動時は入力モード
static GameMode   current_game      = GAME_NONE;
static bool       game_launch_lock  = false;
static bool       play_start_melody = false;      // メロディ再生フラグ

// ============================================================
// ピン定義
// ============================================================
static const pin_t RED_LEDS[8] = { D3, B4, B3, F7, D2, B2, B1, F6 };
#define GREEN_PIN  B5
#define SPEAK_PIN  B6

// ============================================================
// 共通ユーティリティ
// ============================================================

static inline void kb_led_set(uint8_t idx, bool on) {
    writePin(RED_LEDS[idx], on);
}

static inline void leds_clear(void) {
    for (int i = 0; i < 8; i++) writePinLow(RED_LEDS[i]);
    writePinLow(GREEN_PIN);
}

// ============================================================
// QMK オーディオ API ラッパー
// AUDIO_ENABLE = yes + config.h に #define B6_AUDIO が必要
// ============================================================

// SONGの定義（musical_notes.hのマクロを使用）
// Q__NOTE = 四分音符, E__NOTE = 八分音符, H__NOTE = 二分音符
#ifdef AUDIO_ENABLE
float game_start_song[][2]    = { Q__NOTE(_C5), Q__NOTE(_E5), Q__NOTE(_G5), H__NOTE(_C6) };
float mode_switch_song[][2]   = { E__NOTE(_C5), E__NOTE(_E5), E__NOTE(_G5), E__NOTE(_C6), Q__NOTE(_E6) };
float meoshi_happy_song[][2]  = { E__NOTE(_C5), E__NOTE(_E5), E__NOTE(_G5), E__NOTE(_C6), Q__NOTE(_E6), H__NOTE(_G6) };
float meoshi_sad_song[][2]    = { E__NOTE(_A4), Q__NOTE(_F4), E__NOTE(_D4), Q__NOTE(_B3), H__NOTE(_G3) };
float meoshi_clear_song[][2]  = { E__NOTE(_C5), E__NOTE(_E5), E__NOTE(_G5), Q__NOTE(_C6), H__NOTE(_E6) };
float mogura_hit_song[][2]    = { S__NOTE(_D6), S__NOTE(_A6) };
float mogura_miss_song[][2]   = { Q__NOTE(_C3) };
float mogura_gameover_song[][2] = { E__NOTE(_C5), E__NOTE(_E5), E__NOTE(_G5), E__NOTE(_B5), Q__NOTE(_C6) };
float mogura_result_song[][2] = { E__NOTE(_C5), E__NOTE(_C5), E__NOTE(_E5), E__NOTE(_C5),
                                   E__NOTE(_G5), E__NOTE(_A5), Q__NOTE(_C6), E__NOTE(_A5),
                                   E__NOTE(_G5), E__NOTE(_E5), H__NOTE(_C6) };
float roulette_tick_song[][2] = { S__NOTE(_A5) };
float tone_low_song[][2]      = { E__NOTE(_C3) };
#endif

static void play_game_start(void) {
#ifdef AUDIO_ENABLE
    PLAY_SONG(game_start_song);
    wait_ms(700);
#endif
}

static void play_mode_switch_melody(void) {
#ifdef AUDIO_ENABLE
    PLAY_SONG(mode_switch_song);
    wait_ms(600);
#endif
}

static void scan_keys(bool out[8]) {
    for (int i = 0; i < 8; i++) out[i] = matrix_is_on(i / 4, i % 4);
}

static bool check_exit_combo(const bool keys[8]) {
    return keys[2] && keys[3] && keys[6] && keys[7]
        && !keys[0] && !keys[1] && !keys[4] && !keys[5];
}

// ============================================================
// ゲーム1：オルガン
// ============================================================
// キー0〜6 → ドレミファソラシ（C4〜B4）
// キー7（同時押し）→ 1オクターブ上に変調
//
// QMKオーディオAPIを使用。audio_play_note()はノンブロッキングで
// ハードウェアタイマーがバックグラウンドで音を出し続けるため
// 音が完全に連続して綺麗に鳴る。
// ============================================================

// 音程テーブル（musical_notes.hの定義を使用）
static const float ORGAN_NOTES[8] = {
    NOTE_C4, NOTE_D4, NOTE_E4, NOTE_F4, NOTE_G4, NOTE_A4, NOTE_B4, NOTE_C5
};
static const float ORGAN_NOTES_OCT[8] = {
    NOTE_C5, NOTE_D5, NOTE_E5, NOTE_F5, NOTE_G5, NOTE_A5, NOTE_B5, NOTE_C6
};

static bool   organ_key_state[8];
static int8_t organ_active_index  = -1;
static int8_t organ_playing_index = -1;
static bool   organ_oct_playing   = false;

static void init_organ(void) {
    for (int i = 0; i < 8; i++) organ_key_state[i] = false;
    organ_active_index  = -1;
    organ_playing_index = -1;
    organ_oct_playing   = false;
#ifdef AUDIO_ENABLE
    audio_stop_all();
#endif
    leds_clear();
}

static void run_organ(void) {
    bool keys[8];
    scan_keys(keys);

    // 後から押したキーを優先（押下エッジ検出）
    for (int i = 0; i < 7; i++) {
        if (keys[i] && !organ_key_state[i]) organ_active_index = i;
    }

    // LED をキー状態に合わせる
    for (int i = 0; i < 8; i++) {
        if (keys[i] != organ_key_state[i]) {
            organ_key_state[i] = keys[i];
            kb_led_set(i, keys[i]);
        }
    }

    bool oct_shift = keys[7];

    if (organ_active_index >= 0 && !keys[organ_active_index]) organ_active_index = -1;

    if (organ_active_index < 0) {
        for (int i = 0; i < 7; i++) {
            if (keys[i]) { organ_active_index = i; break; }
        }
    }

#ifdef AUDIO_ENABLE
    if (organ_active_index >= 0) {
        // キーまたはオクターブが変わったときだけ音を切り替える
        if (organ_active_index != organ_playing_index || oct_shift != organ_oct_playing) {
            audio_stop_all();
            float note = oct_shift
                ? ORGAN_NOTES_OCT[organ_active_index]
                : ORGAN_NOTES[organ_active_index];
            audio_play_note(note, 0.0f);  // duration=0で押している間ずっと鳴らす
            organ_playing_index = organ_active_index;
            organ_oct_playing   = oct_shift;
        }
    } else {
        if (organ_playing_index >= 0) {
            audio_stop_all();
            organ_playing_index = -1;
        }
    }
#endif
}

// ============================================================
// 蛍PWM（緑LED B5）
// ============================================================
// Timer3 CTCモードで133kHzの割り込みを発生させ、
// ソフトウェアPWMカウンタ(0〜255)と輝度閾値を比較してB5をON/OFF。
// PWM周波数 ≈ 520Hz（チラつきが見えない）。
// 輝度はサイン波（2秒周期）で変化し、ふわっとした蛍の光を実現。
// Timer3はQMK・AUDIOともに未使用なので競合しない。
// ============================================================
static volatile bool    firefly_active   = false;
static volatile uint8_t firefly_pwm_cnt  = 0;    // PWMカウンタ 0〜255
static volatile uint8_t firefly_duty     = 0;    // 現在の輝度 0〜255
static          uint32_t firefly_start_ms = 0;

// Timer3 Compare Match A 割り込み：PWMカウンタ更新とB5制御
ISR(TIMER3_COMPA_vect) {
    if (!firefly_active) {
        writePinLow(GREEN_PIN);
        return;
    }
    firefly_pwm_cnt++;
    writePin(GREEN_PIN, firefly_pwm_cnt < firefly_duty);
}

static void firefly_timer_start(void) {
    TCCR3B = 0;
    TCCR3A = 0;
    TCNT3  = 0;
    OCR3A  = 14;                              // 133kHz割り込み（16MHz/8/(14+1)）
    TIMSK3 = (1 << OCIE3A);
    TCCR3B = (1 << WGM32) | (1 << CS31);     // CTCモード、プリスケーラ8
}

static void firefly_timer_stop(void) {
    TCCR3B = 0;
    TIMSK3 = 0;
    writePinLow(GREEN_PIN);
}

static void firefly_start(void) {
    firefly_active    = true;
    firefly_start_ms  = timer_read32();
    firefly_pwm_cnt   = 0;
    firefly_duty      = 0;
    firefly_timer_start();
}

static void firefly_stop(void) {
    firefly_active = false;
    firefly_timer_stop();
}

// mainループから毎サイクル呼んで輝度を更新（テーブル参照のみ、軽い）
static const uint8_t FIREFLY_TABLE[16] = {
    127, 176, 217, 245, 255, 245, 217, 176, 127, 78, 37, 9, 0, 9, 37, 78
};

static void update_firefly_green(void) {
    if (!firefly_active) return;
    uint32_t t        = timer_elapsed32(firefly_start_ms);
    uint16_t phase_ms = (uint16_t)(t % 2000);
    uint8_t  idx      = (uint8_t)((phase_ms * 16) / 2000);
    firefly_duty      = FIREFLY_TABLE[idx];
}

// ============================================================
// ゲーム2：目押しルーレット
// ============================================================
static const uint8_t ROULETTE_ORDER[9] = {0,1,2,3,8,7,6,5,4};

#define MEOSHI_STOP_KEY   1
#define ROULETTE_SPEED_LEVELS 5
// 5段階の回転スピード（ms）。0=遅い → 4=速い（最速35ms）
static const uint16_t ROULETTE_SPEED_MS_TABLE[ROULETTE_SPEED_LEVELS] = { 60, 52, 44, 37, 30 };

static bool     meoshi_cleared[9];
static int8_t   meoshi_target;
static uint8_t  meoshi_roulette_pos;
static uint8_t  meoshi_speed_level;   // 0〜4、3回成功するごとに増加
static uint8_t  meoshi_success_count; // 現レベルでの成功回数（3回でレベルアップ）
static uint8_t  meoshi_fail_count;    // 連続失敗回数（3回で成功カウントリセット）
static uint32_t meoshi_roulette_timer;
static uint32_t meoshi_blink_timer;
static bool     meoshi_blink_state;
static bool     meoshi_stop_prev;
static bool     meoshi_waiting;    // キー押し待ち状態（再スタート前）

static void meoshi_led(uint8_t pos, bool on) {
    if (pos < 8) kb_led_set(pos, on);
    else         writePin(GREEN_PIN, on);
}

static void play_meoshi_happy(void) {
#ifdef AUDIO_ENABLE
    PLAY_SONG(meoshi_happy_song);
    wait_ms(600);
#endif
}

static void play_meoshi_sad(void) {
#ifdef AUDIO_ENABLE
    PLAY_SONG(meoshi_sad_song);
    wait_ms(900);
#endif
}

static void init_meoshi(void) {
    for (int i = 0; i < 9; i++) meoshi_cleared[i] = false;
    meoshi_target         = 8;  // ターゲットはインデックス8（緑LED）
    meoshi_blink_state    = false;
    meoshi_stop_prev      = false;
    meoshi_waiting        = false;
    // meoshi_speed_level/success/fail は成功・失敗時にのみ変化するのでここではリセットしない
    // meoshi_roulette_pos は停止位置から再スタートするのでリセットしない
    meoshi_roulette_timer = timer_read32();
    meoshi_blink_timer    = timer_read32();
    firefly_stop();
    leds_clear();
}

// ゲーム全体の初回起動時のみ呼ぶ（スタート位置もリセット）
static void init_meoshi_full(void) {
    meoshi_roulette_pos = 8;  // D2（LED[4]、ROULETTE_ORDER[8]=4）からスタート
    init_meoshi();
}

static void run_meoshi(void) {
    bool keys[8];
    scan_keys(keys);

    bool any_key = false;
    for (int i = 0; i < 8; i++) { if (keys[i]) { any_key = true; break; } }

    // ----------------------------------------------------------
    // キー押し待ち状態：いずれかのキーが押されたらルーレット開始
    // ----------------------------------------------------------
    if (meoshi_waiting) {
        update_firefly_green();  // 待機中も蛍PWM更新
        if (any_key && !meoshi_stop_prev) {
            // 停止位置のLEDを消してルーレット開始
            meoshi_led(ROULETTE_ORDER[meoshi_roulette_pos], false);
            firefly_stop();  // 蛍PWM停止
            meoshi_waiting        = false;
            meoshi_roulette_timer = timer_read32();
        }
        meoshi_stop_prev = any_key;
        return;
    }

    uint16_t speed_ms = ROULETTE_SPEED_MS_TABLE[meoshi_speed_level];

    // ルーレット更新
    if (timer_elapsed32(meoshi_roulette_timer) >= speed_ms) {
        uint8_t cur = ROULETTE_ORDER[meoshi_roulette_pos];
        meoshi_led(cur, false);

        meoshi_roulette_pos = (meoshi_roulette_pos + 1) % 9;
        uint8_t nxt = ROULETTE_ORDER[meoshi_roulette_pos];
        meoshi_led(nxt, true);

        // 回転音
#ifdef AUDIO_ENABLE
        PLAY_SONG(roulette_tick_song);
#endif

        meoshi_roulette_timer = timer_read32();
    }

    // ストップキー：いずれかのキーが押されたエッジで停止
    if (any_key && !meoshi_stop_prev) {
        uint8_t stopped_led = ROULETTE_ORDER[meoshi_roulette_pos];

        if (stopped_led == (uint8_t)meoshi_target) {
            // 成功：点灯→happy音→蛍PWM開始
            meoshi_led((uint8_t)meoshi_target, true);
            play_meoshi_happy();  // happy音終了後にfirefly開始
            firefly_start();
            meoshi_fail_count = 0;
            meoshi_success_count++;
            if (meoshi_success_count >= 3 && meoshi_speed_level < ROULETTE_SPEED_LEVELS - 1) {
                meoshi_speed_level++;
                meoshi_success_count = 0;
            }
            // 赤LEDのみ消灯（緑LEDはfirefly_activeに任せる）
            wait_ms(300);
            for (int i = 0; i < 8; i++) kb_led_set(i, false);
        } else {
            // 失敗：一瞬点灯→sad音、3回失敗で成功カウントリセット
            meoshi_led(stopped_led, true);
            play_meoshi_sad();
            meoshi_led(stopped_led, false);
            meoshi_fail_count++;
            if (meoshi_fail_count >= 3) {
                meoshi_success_count = 0;
                meoshi_fail_count    = 0;
            }
            // 全LED消灯して停止位置を点灯
            wait_ms(300);
            leds_clear();
            meoshi_led(ROULETTE_ORDER[meoshi_roulette_pos], true);
        }

        meoshi_waiting   = true;
        meoshi_stop_prev = true;  // キー離し待ち
        return;
    }
    meoshi_stop_prev = any_key;
}

// ============================================================
// ゲーム3：早押しゲーム（表示はキーが押されるまで消えない）
// ============================================================
#define MOGURA_GAME_TIME_SEC 30
#define MOGURA_INIT_WAIT_MS  500

static bool     mogura_active;
static int      mogura_score;
static int8_t   mogura_target[3];      // 同時に表示するキー（最大3つ）
static uint8_t  mogura_target_count;   // 現在の同時押し数（1〜3）
static bool     mogura_led_on;
static uint32_t mogura_start_time;
static uint32_t mogura_next_spawn;
static bool     mogura_prev_keys[8];   // 前サイクルのキー状態（エッジ検出用）

static void init_mogura(void);  // 前方宣言

static void play_mogura_hit(void) {
#ifdef AUDIO_ENABLE
    PLAY_SONG(mogura_hit_song);
#endif
}

static void play_mogura_gameover(void) {
#ifdef AUDIO_ENABLE
    PLAY_SONG(mogura_result_song);
    wait_ms(1500);
#endif
}

static void mogura_spawn(void) {
    uint32_t elapsed_sec = timer_elapsed32(mogura_start_time) / 1000;
    
    // 難易度に応じて同時押し数を決定（1つ→2つ→3つ）
    // 0-10秒: 1つ、10-20秒: 2つ、20秒以降: 3つ
    if (elapsed_sec < 10) {
        mogura_target_count = 1;
    } else if (elapsed_sec < 20) {
        mogura_target_count = 2;
    } else {
        mogura_target_count = 3;
    }
    
    // 重複しないようにランダムにキーを選択
    bool used[8] = {false};
    for (uint8_t i = 0; i < mogura_target_count; i++) {
        int8_t candidate;
        do {
            candidate = rand() % 8;
        } while (used[candidate]);
        used[candidate] = true;
        mogura_target[i] = candidate;
        kb_led_set((uint8_t)candidate, true);
    }
    
    mogura_led_on     = true;
    mogura_next_spawn = timer_read32() + 200;  // 次の出現までの待機時間
}

static void mogura_end(void) {
    mogura_active = false;
    mogura_led_on = false;
    leds_clear();

    play_mogura_gameover();
    wait_ms(200);

    // 5成功ごとにLEDが1つ点灯（最大8個点灯 = 40成功まで）
    int led_count = mogura_score / 5;
    if (led_count > 8) led_count = 8;

    // 1点以上の場合のみ3回点滅
    if (led_count > 0) {
        for (uint8_t blink = 0; blink < 3; blink++) {
            for (int i = 0; i < led_count; i++) kb_led_set(i, true);
            wait_ms(400);
            leds_clear();
            wait_ms(200);
        }
#ifdef AUDIO_ENABLE
        PLAY_SONG(mogura_gameover_song);
        wait_ms(1000);
#endif
    }

    // 自動で再スタート
    init_mogura();
}

static void init_mogura(void) {
    mogura_active       = true;
    mogura_score        = 0;
    mogura_target_count = 0;
    for (int i = 0; i < 3; i++) mogura_target[i] = -1;
    mogura_led_on       = false;
    mogura_start_time   = timer_read32();
    mogura_next_spawn   = timer_read32() + MOGURA_INIT_WAIT_MS;
    for (int i = 0; i < 8; i++) mogura_prev_keys[i] = false;
    leds_clear();
}

static void run_mogura(void) {
    if (!mogura_active) return;

    uint32_t elapsed_sec = timer_elapsed32(mogura_start_time) / 1000;

    if (elapsed_sec >= MOGURA_GAME_TIME_SEC) {
        // ゲーム終了：全てのLEDを消灯
        if (mogura_led_on) {
            for (uint8_t i = 0; i < mogura_target_count; i++) {
                if (mogura_target[i] >= 0) {
                    kb_led_set((uint8_t)mogura_target[i], false);
                }
            }
        }
        mogura_end();
        return;
    }

    // 表示はキーが押されるまで消えない（時間制限なし）

    bool keys[8];
    scan_keys(keys);
    
    // mogura_next_spawn が過去の時刻で、LEDが消えている場合のみ次のキーを出現させる
    if (!mogura_led_on && timer_elapsed32(mogura_next_spawn) < 0x80000000UL) {
        mogura_spawn();
    }

    // キー入力チェック
    if (mogura_led_on && mogura_target_count > 0) {
        // ターゲット以外のキーの押下エッジを検出したら減点＆次のターゲットへ
        bool missed = false;
        for (uint8_t k = 0; k < 8; k++) {
            if (!keys[k] || mogura_prev_keys[k]) continue;  // 押下エッジのみ
            bool is_target = false;
            for (uint8_t i = 0; i < mogura_target_count; i++) {
                if (mogura_target[i] == (int8_t)k) { is_target = true; break; }
            }
            if (!is_target) {
                missed = true;
                break;
            }
        }

        if (missed) {
            // ミス：減点してLED消灯、インターバルを挟んで次のターゲットへ
            if (mogura_score > 0) mogura_score--;
#ifdef AUDIO_ENABLE
            PLAY_SONG(mogura_miss_song);
#endif
            for (uint8_t i = 0; i < mogura_target_count; i++) {
                if (mogura_target[i] >= 0) {
                    kb_led_set((uint8_t)mogura_target[i], false);
                }
            }
            mogura_led_on     = false;
            mogura_next_spawn = timer_read32() + 400;  // 成功より少し長めのインターバル
        } else {
            // 全てのターゲットが同時に押されているか確認
            bool all_pressed = true;
            for (uint8_t i = 0; i < mogura_target_count; i++) {
                if (mogura_target[i] < 0 || !keys[(uint8_t)mogura_target[i]]) {
                    all_pressed = false;
                    break;
                }
            }

            if (all_pressed) {
                // 成功：加点してLED消灯、インターバルを挟んで次のターゲットへ
                play_mogura_hit();
                mogura_score++;
                for (uint8_t i = 0; i < mogura_target_count; i++) {
                    if (mogura_target[i] >= 0) {
                        kb_led_set((uint8_t)mogura_target[i], false);
                    }
                }
                mogura_led_on     = false;
                mogura_next_spawn = timer_read32() + 200;
            }
        }
    }

    // キー状態を保存（次サイクルのエッジ検出用）
    for (uint8_t i = 0; i < 8; i++) mogura_prev_keys[i] = keys[i];
}

// ============================================================
// モード切り替えタイマー
// ============================================================
static uint32_t mode_switch_timer = 0;
static bool     mode_switch_held  = false;
static uint32_t keyboard_clear_timer = 0;  // キーボードクリア用タイマー

// ============================================================
// QMK フック：keyboard_post_init_user
// ============================================================
void keyboard_post_init_user(void) {
    for (int i = 0; i < 8; i++) setPinOutput(RED_LEDS[i]);
    setPinOutput(GREEN_PIN);
    // SPEAK_PIN(B6)はAUDIO_ENABLEが管理するため設定不要
    leds_clear();
    srand(timer_read32());
}

// ============================================================
// QMK フック：matrix_scan_user（メインループ）
//
// 【USB安定性の設計原則】
//   GAME_NONE 状態（ゲーム未起動）では wait_us / wait_ms を呼ばない。
//   これにより Remap のテストマトリクスが正常に動作する。
//   ゲーム起動メロディは play_start_melody フラグで管理し、
//   current_game が GAME_NONE → 実ゲームに切り替わった直後のサイクルで
//   1回だけ鳴らす（その間はゲーム中扱いなのでUSBブロックを許容）。
// ============================================================
void matrix_scan_user(void) {
    bool keys[8];
    scan_keys(keys);

    // ----------------------------------------------------------
    // 【最優先】ゲーム起動メロディの再生（フラグが立っている時のみ）
    // current_game は既に切り替わった後なので「ゲーム中」として扱える
    // ----------------------------------------------------------
    if (play_start_melody) {
        play_start_melody = false;
        play_game_start();
        return;
    }

    // ----------------------------------------------------------
    // モード切り替え検出（0+1+4+5を2秒長押し）
    // ----------------------------------------------------------
    bool mode_combo = keys[0] && keys[1] && keys[4] && keys[5]
                   && !keys[2] && !keys[3] && !keys[6] && !keys[7];

    if (mode_combo) {
        if (!mode_switch_held) {
            mode_switch_timer = timer_read32();
            mode_switch_held  = true;
        }
        if (timer_elapsed32(mode_switch_timer) >= 2000) {
            current_mode = (current_mode == MODE_GAME) ? MODE_INPUT : MODE_GAME;
            current_game = GAME_NONE;
            leds_clear();
            play_mode_switch_melody(); // モード切り替え音はここでブロッキングOK
            if (current_mode == MODE_GAME) game_launch_lock = true;
            mode_switch_held = false;
            wait_ms(500);
        }
        return;
    } else {
        mode_switch_held = false;
    }

    // ロック解除（コンビキーが全部離れたら）
    if (game_launch_lock) {
        if (!keys[0] && !keys[1] && !keys[2] && !keys[3]
         && !keys[4] && !keys[5] && !keys[6] && !keys[7]) {
            game_launch_lock = false;
        }
        return;
    }

    // 入力モードはQMK通常処理に任せる
    if (current_mode == MODE_INPUT) return;

    // ----------------------------------------------------------
    // ゲームモード：終了コンビ（2+3+6+7）チェック
    // ----------------------------------------------------------
    if (current_game != GAME_NONE && check_exit_combo(keys)) {
        clear_keyboard();  // 終了時にも全てのキーをリリース
        firefly_stop();    // 蛍PWM停止
        current_game = GAME_NONE;
        keyboard_clear_timer = timer_read32();
        leds_clear();
#ifdef AUDIO_ENABLE
        PLAY_SONG(tone_low_song);
        wait_ms(300);
#endif
        return;
    }

    // ----------------------------------------------------------
    // ゲーム選択（キー0+1, 0+2, 0+3）
    // ここでは wait_us/wait_ms を使わない（テストマトリクス動作中の可能性）
    // ----------------------------------------------------------
    if (current_game == GAME_NONE) {
        // 押しているキーに対応するLEDを点灯（ノンブロッキング）
        for (int i = 0; i < 8; i++) kb_led_set(i, keys[i]);

        if (keys[0] && keys[1] && !keys[2] && !keys[3]
         && !keys[4] && !keys[5] && !keys[6] && !keys[7]) {
            leds_clear();
            clear_keyboard();  // 全てのキーをリリース
            keyboard_clear_timer = timer_read32();  // タイマーリセット
            init_organ();
            current_game      = GAME_ORGAN;
            play_start_melody = true; // 次サイクルでメロディを鳴らす
            game_launch_lock  = true;
        } else if (keys[0] && keys[2] && !keys[1] && !keys[3]
                && !keys[4] && !keys[5] && !keys[6] && !keys[7]) {
            leds_clear();
            clear_keyboard();  // 全てのキーをリリース
            keyboard_clear_timer = timer_read32();  // タイマーリセット
            meoshi_speed_level   = 0;  // ゲーム開始時はスピード最遅
            meoshi_success_count = 0;
            meoshi_fail_count    = 0;
            init_meoshi_full();  // 初回はスタート位置もリセット
            current_game      = GAME_MEOSHI;
            play_start_melody = true;
            game_launch_lock  = true;
        } else if (keys[0] && keys[3] && !keys[1] && !keys[2]
                && !keys[4] && !keys[5] && !keys[6] && !keys[7]) {
            leds_clear();
            clear_keyboard();  // 全てのキーをリリース
            keyboard_clear_timer = timer_read32();  // タイマーリセット
            init_mogura();
            current_game      = GAME_MOGURA;
            play_start_melody = true;
            game_launch_lock  = true;
        }
        return;
    }

    // ----------------------------------------------------------
    // ゲーム実行中：定期的にキーボードをクリアして誤送信を防ぐ
    // ----------------------------------------------------------
    if (current_game != GAME_NONE) {
        // 100msごとにキーボードをクリア（誤送信されたキーを確実にリリース）
        if (timer_elapsed32(keyboard_clear_timer) >= 100) {
            clear_keyboard();
            keyboard_clear_timer = timer_read32();
        }
    } else {
        keyboard_clear_timer = timer_read32();
    }

    // ----------------------------------------------------------
    // ゲーム実行
    // ----------------------------------------------------------
    switch (current_game) {
        case GAME_ORGAN:  run_organ();  break;
        case GAME_MEOSHI: run_meoshi(); break;
        case GAME_MOGURA:
            run_mogura();
            break;
        default: break;
    }
}

// ============================================================
// QMK フック：process_record_user
//
// 【RemapテストマトリクスとHID送信の両立】
// VIA/Remapのテストマトリクスは matrix_get_row() でマトリクス状態を読む。
// process_record_user の戻り値には直接依存しないため、
// ゲームモード中に false を返しても テストマトリクスは正常に動作する。
//
// ゲームモード中にキーボードとして文字を送信させないため return false。
// 入力モードでは通常通り return true。
// ============================================================
bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    // ゲームモード中は一切のキー入力を無効化（HID送信をブロック）
    // ゲーム選択のコンビキーは matrix_scan_user() で直接マトリクスを読むため
    // process_record_user() で false を返しても動作する
    if (current_mode == MODE_GAME) {
        return false;  // ゲームモード中は常にキー入力をブロック
    }

    // 入力モード：LED制御して通常送信
    uint8_t idx = record->event.key.row * 4 + record->event.key.col;
    if (idx < 8) kb_led_set(idx, record->event.pressed);
    return true;
}

// ============================================================
// QMK フック：layer_state_set_user
// ============================================================
layer_state_t layer_state_set_user(layer_state_t state) {
    if (current_mode == MODE_INPUT) {
        writePin(GREEN_PIN, get_highest_layer(state) > 0);
    }
    return state;
}

// ============================================================
// キーマップ
// ============================================================
const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [_BASE]   = LAYOUT(KC_W,   KC_A,    KC_S,             LT(_LAYER3, KC_D),
                       KC_SPC, KC_LSFT, LT(_LAYER2, KC_E), LT(_LAYER1, KC_1)),
    [_LAYER1] = LAYOUT(KC_2, KC_3, KC_4, KC_5,
                       KC_6, KC_7, KC_8, KC_9),
    [_LAYER2] = LAYOUT(KC_Q,   KC_R,   KC_F, KC_TAB,
                       KC_ENT, KC_ESC, KC_T, KC_C),
    [_LAYER3] = LAYOUT(KC_LEFT, KC_UP, KC_DOWN, KC_RIGHT,
                       KC_0,    KC_1,  KC_2,    KC_SPC),
};
