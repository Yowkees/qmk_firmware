/*
 * Keybit8 QMK Firmware - keymap.c  v3
 * ゲーム統合版：オルガン / 目押しルーレット / 早押しゲーム
 *
 * [Remapテストマトリクス対応について]
 *   RemapのVIAテストマトリクスは matrix_get_row() 経由でマトリクス状態を読む。
 *   process_record_user の戻り値には影響されないが、
 *   matrix_scan_user 内でブロッキング処理（wait_us/wait_ms）を行うと
 *   USBポーリング(1ms間隔)が妨害され、VIA通信ごとタイムアウトする。
 *
 *   解決策：
 *   - GAME_NONE（ゲーム未選択）状態では wait_us/wait_ms を一切使わない
 *   - ゲーム起動メロディは process_record_user 経由のワンショットフラグで
 *     matrix_scan_user の最初に1回だけ鳴らす（ゲーム中なのでUSBは止まってもよい）
 *   - オルガンの音生成は wait_us を使うが、それはゲーム中（GAME_ORGAN）の時だけ
 *   - 効果音（hit/miss等）もゲーム中にのみ発生するため問題なし
 *
 * ゲーム起動コンビ：キー0+1=オルガン / 0+2=目押しルーレット / 0+3=早押しゲーム
 * モード切り替え：0+1+4+5を2秒長押し（ゲーム⇔入力）
 * ゲーム終了：2+3+6+7同時押し
 */

#include QMK_KEYBOARD_H
#include <stdlib.h>

// ============================================================
// レイヤー定義
// ============================================================
enum layer_names { _BASE, _LAYER1, _LAYER2, _LAYER3 };

// ============================================================
// モード・ゲーム定義
// ============================================================
typedef enum { MODE_GAME, MODE_INPUT } SystemMode;
typedef enum { GAME_NONE, GAME_ORGAN, GAME_MEOSHI, GAME_MOGURA } GameMode;

static SystemMode current_mode      = MODE_GAME;
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

/*
 * play_tone_blocking()
 * ゲーム中（current_game != GAME_NONE）にのみ呼ぶこと。
 * USBポーリングをブロックするが、ゲーム中はそれを許容する。
 */
static void play_tone_blocking(uint16_t freq_hz, uint16_t duration_ms) {
    if (freq_hz == 0) { wait_ms(duration_ms); return; }
    uint32_t half_us = 500000UL / freq_hz;
    uint32_t end_us  = (uint32_t)duration_ms * 1000UL;
    uint32_t elapsed = 0;
    while (elapsed < end_us) {
        writePinHigh(SPEAK_PIN);
        wait_us(half_us);
        writePinLow(SPEAK_PIN);
        wait_us(half_us);
        elapsed += half_us * 2;
    }
}

static void play_melody(const uint16_t *freqs, const uint16_t *durs, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        play_tone_blocking(freqs[i], durs[i]);
        wait_ms(20);
    }
    writePinLow(SPEAK_PIN);
}

static void play_game_start(void) {
    static const uint16_t f[] = {523, 659, 784, 1047};
    static const uint16_t d[] = {120, 120, 120, 240};
    play_melody(f, d, 4);
}

static void play_mode_switch_melody(void) {
    static const uint16_t f[] = {523, 659, 784, 1047, 1319};
    static const uint16_t d[] = {100, 100, 100, 100,  200};
    play_melody(f, d, 5);
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
// matrix_scan_user から毎回呼ばれる想定の非ブロッキング風実装
// （元の実装に合わせて音程を安定させる）
static const uint16_t ORGAN_NOTES[8] = {262, 294, 330, 349, 392, 440, 494, 523};
static bool   organ_key_state[8];   // 現在押されているキー状態
static int8_t organ_active_index = -1; // 現在鳴らしているキー（後押し優先）

static void init_organ(void) {
    for (int i = 0; i < 8; i++) organ_key_state[i] = false;
    organ_active_index = -1;
    leds_clear();
}

static void run_organ(void) {
    bool keys[8];
    scan_keys(keys);

    // 後から押したキーを優先するため、押下エッジを検出して active を更新
    for (int i = 0; i < 7; i++) {
        if (keys[i] && !organ_key_state[i]) {
            // 新しく押されたキーをアクティブにする
            organ_active_index = i;
        }
    }

    // LED をキー状態に合わせる
    for (int i = 0; i < 8; i++) {
        if (keys[i] != organ_key_state[i]) {
            organ_key_state[i] = keys[i];
            kb_led_set(i, keys[i]);
        }
    }

    // キー7 = オクターブシフト（音は出さない）
    bool oct_shift = keys[7];

    // アクティブキーが離されていたらリセット
    if (organ_active_index >= 0 && !keys[organ_active_index]) {
        organ_active_index = -1;
    }

    // 何もアクティブでないが、キーは押されている場合のフォールバック
    if (organ_active_index < 0) {
        for (int i = 0; i < 7; i++) {
            if (keys[i]) {
                organ_active_index = i;
                break;
            }
        }
    }

    if (organ_active_index >= 0) {
        uint16_t freq = ORGAN_NOTES[organ_active_index];
        if (oct_shift) freq *= 2;

        // 周波数から半周期を直接算出して 50% デューティ比で出力
        // （元の実装と同じ方式で、音程が最も安定する）
        uint32_t half_us = 500000UL / freq;
        writePinHigh(SPEAK_PIN);
        wait_us(half_us);
        writePinLow(SPEAK_PIN);
        wait_us(half_us);
    } else {
        writePinLow(SPEAK_PIN);
    }
}

// ============================================================
// ゲーム2：目押しルーレット
// ============================================================
static const uint8_t ROULETTE_ORDER[9] = {0,1,2,3,8,7,6,5,4};

#define MEOSHI_STOP_KEY   1
#define ROULETTE_SPEED_LEVELS 5
// 5段階の回転スピード（ms）。0=遅い → 4=速い
static const uint16_t ROULETTE_SPEED_MS_TABLE[ROULETTE_SPEED_LEVELS] = { 120, 100, 80, 65, 50 };

static bool     meoshi_cleared[9];
static int8_t   meoshi_target;
static uint8_t  meoshi_roulette_pos;
static uint8_t  meoshi_speed_level;   // 0〜4、成功するごとに増加
static uint32_t meoshi_roulette_timer;
static uint32_t meoshi_blink_timer;
static bool     meoshi_blink_state;
static bool     meoshi_stop_prev;

static void meoshi_led(uint8_t pos, bool on) {
    if (pos < 8) kb_led_set(pos, on);
    else         writePin(GREEN_PIN, on);
}

static void play_meoshi_happy(void) {
    static const uint16_t f[] = {800, 1000, 1200};
    static const uint16_t d[] = {150,  150,  300};
    play_melody(f, d, 3);
}

static void play_meoshi_sad(void) {
    static const uint16_t f[] = {400, 300, 200};
    static const uint16_t d[] = {200, 300, 400};
    play_melody(f, d, 3);
}

static void init_meoshi(void) {
    for (int i = 0; i < 9; i++) meoshi_cleared[i] = false;
    meoshi_target         = 8;  // ターゲットはインデックス8（緑LEDだが他と同じように回転させる）
    meoshi_roulette_pos   = 0;
    meoshi_blink_state    = false;
    meoshi_stop_prev      = false;
    // meoshi_speed_level は成功時にのみ増加するのでここではリセットしない
    meoshi_roulette_timer = timer_read32();
    meoshi_blink_timer    = timer_read32();
    leds_clear();
}

static void run_meoshi(void) {
    bool keys[8];
    scan_keys(keys);

    uint16_t speed_ms = ROULETTE_SPEED_MS_TABLE[meoshi_speed_level];

    // ルーレット更新
    if (timer_elapsed32(meoshi_roulette_timer) >= speed_ms) {
        uint8_t cur = ROULETTE_ORDER[meoshi_roulette_pos];
        meoshi_led(cur, false);

        meoshi_roulette_pos = (meoshi_roulette_pos + 1) % 9;
        uint8_t nxt = ROULETTE_ORDER[meoshi_roulette_pos];
        meoshi_led(nxt, true);

        // 回転音：delayTime 相当を speed_ms とみなし、20〜200ms → 1200〜400Hz にマップ
        int delay_ms = (int)speed_ms;
        if (delay_ms < 20) delay_ms = 20;
        if (delay_ms > 200) delay_ms = 200;
        int pitch = 1200 - (delay_ms - 20) * 800 / 180;
        play_tone_blocking((uint16_t)pitch, (uint16_t)(delay_ms / 2));

        meoshi_roulette_timer = timer_read32();
    }

    // ストップキー（キー1）エッジ検出
    bool stop_now = keys[MEOSHI_STOP_KEY];
    if (stop_now && !meoshi_stop_prev) {
        uint8_t stopped_led = ROULETTE_ORDER[meoshi_roulette_pos];

        if (stopped_led == (uint8_t)meoshi_target) {
            // 成功：点灯→happy音、スピードレベルを1段階上げ（最大4）
            meoshi_led((uint8_t)meoshi_target, true);
            play_meoshi_happy();
            if (meoshi_speed_level < ROULETTE_SPEED_LEVELS - 1) {
                meoshi_speed_level++;
            }
        } else {
            // 失敗：一瞬点灯→sad音（スピードレベルはそのまま）
            meoshi_led(stopped_led, true);
            play_meoshi_sad();
            meoshi_led(stopped_led, false);
        }
        
        // 成功・失敗に関わらずゲーム終了して再スタート
        wait_ms(500);
        leds_clear();
        init_meoshi();  // 再初期化してルーレットを再開
    }
    meoshi_stop_prev = stop_now;
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

static void play_mogura_hit(void) {
    play_tone_blocking(1200, 30);
    play_tone_blocking(1500, 20);
    writePinLow(SPEAK_PIN);
}

static void play_mogura_gameover(void) {
    // 明るい結果表示用メロディ（Cメジャー上昇フレーズ）
    static const uint16_t f[] = {523, 659, 784, 988, 1047};
    static const uint16_t d[] = {130, 130, 130, 130, 200};
    play_melody(f, d, 5);
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
    wait_ms(500);

    // 3成功ごとにLEDが1つ点灯（最大9個点灯 = 27成功まで）
    int led_count = mogura_score / 3;
    if (led_count > 9) led_count = 9;  // 最大9個まで
    
    if (led_count == 0) {
        // 0〜2成功：全LEDを一瞬点灯して終わり
        for (int i = 0; i < 8; i++) kb_led_set(i, true);
        wait_ms(400);
        leds_clear();
    } else {
        // 3成功ごとにLEDを点灯（左から順に）
        for (int i = 0; i < led_count && i < 8; i++) {
            kb_led_set(i, true);
        }
        // 9個目の場合は緑LEDも点灯
        if (led_count == 9) {
            writePinHigh(GREEN_PIN);
        }
        play_tone_blocking(600, 300);
        wait_ms(2000);
        leds_clear();
    }
    wait_ms(1000);
    leds_clear();
}

static void init_mogura(void) {
    mogura_active       = true;
    mogura_score        = 0;
    mogura_target_count = 0;
    for (int i = 0; i < 3; i++) mogura_target[i] = -1;
    mogura_led_on       = false;
    mogura_start_time   = timer_read32();
    mogura_next_spawn   = timer_read32() + MOGURA_INIT_WAIT_MS;
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

    // キー入力チェック：全てのターゲットが同時に押されているか確認
    if (mogura_led_on && mogura_target_count > 0) {
        bool all_pressed = true;
        for (uint8_t i = 0; i < mogura_target_count; i++) {
            if (mogura_target[i] < 0 || !keys[(uint8_t)mogura_target[i]]) {
                all_pressed = false;
                break;
            }
        }
        
        if (all_pressed) {
            // 全てのターゲットが同時に押された：成功
            play_mogura_hit();
            mogura_score++;
            // 全てのLEDを消灯
            for (uint8_t i = 0; i < mogura_target_count; i++) {
                if (mogura_target[i] >= 0) {
                    kb_led_set((uint8_t)mogura_target[i], false);
                }
            }
            mogura_led_on = false;
            mogura_next_spawn = timer_read32() + 200;  // すぐに次のキーを出現
        }
    }
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
    setPinOutput(SPEAK_PIN);
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
        current_game = GAME_NONE;
        keyboard_clear_timer = timer_read32();
        leds_clear();
        play_tone_blocking(200, 200); // 終了音（ゲーム中なのでブロッキングOK）
        wait_ms(300);
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
            init_meoshi();
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
            if (!mogura_active) current_game = GAME_NONE;
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
