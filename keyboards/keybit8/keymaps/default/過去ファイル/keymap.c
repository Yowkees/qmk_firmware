/*
 * Keybit8 QMK Firmware - keymap.c
 * ゲーム統合版：オルガン / 目押しルーレット / モグラ叩き
 *
 * ゲーム起動コンビ（ゲームモード中）：
 *   キー0+1 → オルガンゲーム
 *   キー0+2 → 目押しルーレット
 *   キー0+3 → モグラ叩き
 *
 * モード切り替え（0+1+4+5を2秒長押し）：
 *   ゲームモード ⇔ 入力モード（キーボード）
 *
 * ゲーム中の終了（2+3+6+7同時押し）：
 *   どのゲームでも終了してゲーム選択待ちに戻る
 *
 * ピン定義（config.h準拠）：
 *   赤LED[0..7] = { D3, B4, B3, F7, D2, B2, B1, F6 }
 *   緑LED        = B5
 *   スピーカー   = B6
 *   キーマトリクス：ROW={F4,F5}, COL={D4,C6,D7,E6}
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

static SystemMode current_mode = MODE_GAME;
static GameMode   current_game = GAME_NONE;
static bool       game_launch_lock = false; // 起動コンビのキーが離されるまでロック

// ============================================================
// ピン定義（config.h の RED_LED_PINS / GREEN_LED_PIN / SPEAKER_PIN に対応）
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
 * スピーカーをGPIOで直接駆動してビープを鳴らす。
 * QMK の AUDIO_ENABLE は使わず、rules.mk の設定のまま動作する。
 * ※ ATmega32U4 ではハードウェアタイマーを使うとQMKの内部処理と競合する
 *   恐れがあるため、ここではソフトウェアループで生成する。
 *   オルガンのような「押している間ずっと鳴らす」には別途対応済み（後述）。
 */
static void play_tone_blocking(uint16_t freq_hz, uint16_t duration_ms) {
    if (freq_hz == 0) { wait_ms(duration_ms); return; }
    // 半周期 (us)
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

// 短いメロディをまとめて鳴らすヘルパー
static void play_melody(const uint16_t *freqs, const uint16_t *durs, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        play_tone_blocking(freqs[i], durs[i]);
        wait_ms(20);
    }
    writePinLow(SPEAK_PIN);
}

// ゲーム開始共通メロディ
static void play_game_start(void) {
    static const uint16_t f[] = {523, 659, 784, 1047};
    static const uint16_t d[] = {120, 120, 120, 240};
    play_melody(f, d, 4);
}

// モード切り替えメロディ
static void play_mode_switch(void) {
    static const uint16_t f[] = {523, 659, 784, 1047, 1319};
    static const uint16_t d[] = {100, 100, 100, 100,  200 };
    play_melody(f, d, 5);
}

// 全キー押下状態をまとめて取得（キーインデックス = row*4 + col）
static void scan_keys(bool out[8]) {
    for (int i = 0; i < 8; i++) out[i] = matrix_is_on(i / 4, i % 4);
}

// ============================================================
// ゲーム終了チェック共通（2+3+6+7同時押し）
// キーインデックス：2=row0col2, 3=row0col3, 6=row1col2, 7=row1col3
// ============================================================
static bool check_exit_combo(const bool keys[8]) {
    return keys[2] && keys[3] && keys[6] && keys[7]
        && !keys[0] && !keys[1] && !keys[4] && !keys[5];
}

// ============================================================
// ゲーム1：オルガン
// ============================================================
// キー0〜6 → ドレミファソラシ（C4〜B4）
// キー7（同時押し）→ 1オクターブ上に変調
// 押している間ずっと音を出す：matrix_scan_user の中で毎回スピーカーを駆動する
// ※ ブロッキングではなく「1サイクルごとに1/2周期だけ出力」する方式で擬似的に実現

static const uint16_t ORGAN_NOTES[8] = {262, 294, 330, 349, 392, 440, 494, 523};

// オルガン用状態
static bool organ_key_state[8]; // 現在押されているキー状態

static void init_organ(void) {
    for (int i = 0; i < 8; i++) organ_key_state[i] = false;
    leds_clear();
}

/*
 * run_organ()
 * matrix_scan_user から毎回呼ばれる（非ブロッキング想定）。
 * スピーカーは「押している間ずっと鳴る」を実現するため、
 * 有効キーがあれば半周期 HIGH → 半周期 LOW を1サイクル出力する。
 * matrix_scan_user のコールレートは ~1kHz 程度なので、
 * 音域の周波数（262〜1046 Hz）に対してサイクルが多少間引かれるが
 * 実用上は問題ない音として聴こえる。
 */
static void run_organ(void) {
    bool keys[8];
    scan_keys(keys);

    // LEDをキー状態に合わせる
    for (int i = 0; i < 8; i++) {
        if (keys[i] != organ_key_state[i]) {
            organ_key_state[i] = keys[i];
            kb_led_set(i, keys[i]);
        }
    }

    // キー7 = オクターブシフト（音は出さない）
    bool oct_shift = keys[7];

    // 最低インデックスの押されているキー（キー7以外）を発音対象にする
    int active = -1;
    for (int i = 0; i < 7; i++) {
        if (keys[i]) { active = i; break; }
    }

    if (active >= 0) {
        uint16_t freq = ORGAN_NOTES[active];
        if (oct_shift) freq *= 2;
        // 1/2周期だけ出力（HIGH→LOW）
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
// ルール：
//   - LEDが外周（順番 0→1→2→3→8(緑)→7→6→5→4）を循環する
//   - 同時に1つの位置が「ターゲット」として点滅する
//   - キー1（キーインデックス1）を押してルーレットがターゲット位置で
//     止まれば成功 → その位置が恒久点灯に変わる
//   - 全9箇所成功でクリア（ファンファーレ＋全点滅）
//   - 外れたら悲しい音、外した後は再びルーレットが回る
//
// ゲーム終了（2+3+6+7）は共通処理で対応

static const uint8_t ROULETTE_ORDER[9] = {0,1,2,3,8,7,6,5,4};
// インデックス8 = 緑LED扱い

#define MEOSHI_STOP_KEY 1          // キーインデックス1でストップ
#define ROULETTE_SPEED_MS 80       // ルーレット1ステップの間隔(ms)
#define BLINK_INTERVAL_MS 100      // ターゲット点滅間隔(ms)

static bool     meoshi_cleared[9];
static int8_t   meoshi_target;           // ターゲット位置 (-1=未設定)
static uint8_t  meoshi_roulette_pos;     // ルーレット現在位置(ROULETTE_ORDERのインデックス)
static bool     meoshi_running;          // ルーレット回転中フラグ
static uint32_t meoshi_roulette_timer;
static uint32_t meoshi_blink_timer;
static bool     meoshi_blink_state;
static bool     meoshi_stop_prev;        // 前フレームのキー1状態（エッジ検出用）

// インデックス8(緑LED)も含めたLED制御ヘルパー
static void meoshi_led(uint8_t pos, bool on) {
    if (pos < 8) {
        kb_led_set(pos, on);
    } else {
        writePin(GREEN_PIN, on);
    }
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

static void play_meoshi_clear(void) {
    static const uint16_t f[] = {523, 659, 784, 1047, 1319};
    static const uint16_t d[] = {100, 100, 100,  150,  200};
    play_melody(f, d, 5);
}

static void meoshi_pick_target(void) {
    // 未クリアの位置からランダムに選ぶ
    uint8_t pool[9];
    uint8_t cnt = 0;
    for (int i = 0; i < 9; i++) {
        if (!meoshi_cleared[i]) pool[cnt++] = i;
    }
    if (cnt == 0) { meoshi_target = -1; return; }
    meoshi_target = pool[rand() % cnt];
    meoshi_blink_state = false;
}

static void init_meoshi(void) {
    for (int i = 0; i < 9; i++) meoshi_cleared[i] = false;
    meoshi_target       = -1;
    meoshi_roulette_pos = 0;
    meoshi_running      = true;
    meoshi_blink_state  = false;
    meoshi_stop_prev    = false;
    meoshi_roulette_timer = timer_read32();
    meoshi_blink_timer    = timer_read32();
    leds_clear();
}

static void run_meoshi(void) {
    bool keys[8];
    scan_keys(keys);

    // ターゲット未設定なら新たに選ぶ
    if (meoshi_target < 0) meoshi_pick_target();

    // ---------- ルーレット更新 ----------
    if (meoshi_running && timer_elapsed32(meoshi_roulette_timer) >= ROULETTE_SPEED_MS) {
        // 現在位置のLEDを消す（ターゲットは点滅で管理するので触らない）
        uint8_t cur = ROULETTE_ORDER[meoshi_roulette_pos];
        if (cur != (uint8_t)meoshi_target) meoshi_led(cur, false);

        // 次に進む
        meoshi_roulette_pos = (meoshi_roulette_pos + 1) % 9;
        uint8_t nxt = ROULETTE_ORDER[meoshi_roulette_pos];
        if (nxt != (uint8_t)meoshi_target) meoshi_led(nxt, true);

        // 回転音（短い高音）
        play_tone_blocking(900, 10);

        meoshi_roulette_timer = timer_read32();
    }

    // ---------- ターゲット点滅 ----------
    if (meoshi_target >= 0 && timer_elapsed32(meoshi_blink_timer) >= BLINK_INTERVAL_MS) {
        meoshi_blink_state = !meoshi_blink_state;
        meoshi_led((uint8_t)meoshi_target, meoshi_blink_state);
        meoshi_blink_timer = timer_read32();
    }

    // クリア済みの位置は常時点灯
    for (int i = 0; i < 9; i++) {
        if (meoshi_cleared[i] && i != meoshi_target) {
            meoshi_led(i, true);
        }
    }

    // ---------- ストップキー（キー1）のエッジ検出 ----------
    bool stop_now = keys[MEOSHI_STOP_KEY];
    if (stop_now && !meoshi_stop_prev) {
        // 押した瞬間の処理
        uint8_t stopped_led = ROULETTE_ORDER[meoshi_roulette_pos];

        if (stopped_led == (uint8_t)meoshi_target) {
            // 成功
            meoshi_cleared[meoshi_target] = true;
            meoshi_led((uint8_t)meoshi_target, true);
            play_meoshi_happy();
            meoshi_target = -1; // 次のターゲットを促す

            // 全クリアチェック
            bool all_done = true;
            for (int i = 0; i < 9; i++) {
                if (!meoshi_cleared[i]) { all_done = false; break; }
            }
            if (all_done) {
                play_meoshi_clear();
                // 全点滅5回
                for (int b = 0; b < 5; b++) {
                    for (int i = 0; i < 8; i++) kb_led_set(i, true);
                    writePinHigh(GREEN_PIN);
                    wait_ms(200);
                    leds_clear();
                    wait_ms(200);
                }
                // リセットして続けられるようにする
                for (int i = 0; i < 9; i++) meoshi_cleared[i] = false;
                meoshi_target = -1;
            }
        } else {
            // 失敗（クリア済み位置に止まっても失敗扱い）
            meoshi_led(stopped_led, true);
            play_meoshi_sad();
        }
        wait_ms(500);
    }
    meoshi_stop_prev = stop_now;
}

// ============================================================
// ゲーム3：モグラ叩き
// ============================================================
// ルール：
//   - 30秒間のタイムアタック
//   - ランダムなLEDが点灯（モグラ出現）、時間内に対応するキーを押す
//   - 経過時間でモグラの出現速度が上昇
//   - ゲーム終了後、スコアをLED点滅で表示（10点ごとに1回点滅）

#define MOGURA_GAME_TIME_SEC 30
#define MOGURA_INIT_DUR_MS   800
#define MOGURA_MIN_DUR_MS    300
#define MOGURA_INIT_WAIT_MS  500

static bool     mogura_active;
static int      mogura_score;
static int8_t   mogura_target;      // 現在出ているモグラのキーインデックス (-1=なし)
static bool     mogura_led_on;
static uint32_t mogura_start_time;
static uint32_t mogura_led_on_time;
static uint32_t mogura_duration_ms;
static uint32_t mogura_next_spawn;

static void play_mogura_hit(void) {
    play_tone_blocking(1200, 30);
    play_tone_blocking(1500, 20);
    writePinLow(SPEAK_PIN);
}

static void play_mogura_miss(void) {
    play_tone_blocking(300, 100);
    writePinLow(SPEAK_PIN);
}

static void play_mogura_gameover(void) {
    static const uint16_t f[] = {400, 350, 300, 250, 200};
    static const uint16_t d[] = {150, 150, 150, 150, 150};
    play_melody(f, d, 5);
}

static void mogura_spawn(void) {
    mogura_target   = rand() % 8;
    kb_led_set(mogura_target, true);
    mogura_led_on   = true;
    mogura_led_on_time = timer_read32();

    // 経過時間でわずかに次の出現を早める
    uint32_t elapsed_sec = timer_elapsed32(mogura_start_time) / 1000;
    uint32_t base = (elapsed_sec * 10 < 400) ? (400 - elapsed_sec * 10) : 0;
    if (base < 150) base = 150;
    mogura_next_spawn = timer_read32() + base;
}

static void mogura_end(void) {
    mogura_active = false;
    mogura_led_on = false;
    leds_clear();

    play_mogura_gameover();
    wait_ms(500);

    // スコア表示：10点ごとにキー0のLEDを1回点滅
    int blinks = mogura_score / 10;
    if (blinks == 0) {
        // 0点：全LEDを一瞬点灯して終わり
        for (int i = 0; i < 8; i++) kb_led_set(i, true);
        wait_ms(300);
        leds_clear();
    } else {
        for (int i = 0; i < blinks; i++) {
            kb_led_set(0, true);
            play_tone_blocking(600, 150);
            wait_ms(200);
            leds_clear();
            wait_ms(300);
        }
    }
    wait_ms(1000);
    leds_clear();
}

static void init_mogura(void) {
    mogura_active     = true;
    mogura_score      = 0;
    mogura_target     = -1;
    mogura_led_on     = false;
    mogura_start_time = timer_read32();
    mogura_duration_ms= MOGURA_INIT_DUR_MS;
    mogura_next_spawn = timer_read32() + MOGURA_INIT_WAIT_MS;
    leds_clear();
}

static void run_mogura(void) {
    if (!mogura_active) return;

    // タイムアップ
    uint32_t elapsed_sec = timer_elapsed32(mogura_start_time) / 1000;
    if (elapsed_sec >= MOGURA_GAME_TIME_SEC) {
        if (mogura_led_on) kb_led_set(mogura_target, false);
        mogura_end();
        return;
    }

    // 経過に応じてモグラの表示時間を短縮
    uint32_t new_dur = (elapsed_sec * 10 < 500) ? (MOGURA_INIT_DUR_MS - elapsed_sec * 10) : 0;
    if (new_dur < MOGURA_MIN_DUR_MS) new_dur = MOGURA_MIN_DUR_MS;
    if (new_dur < mogura_duration_ms) mogura_duration_ms = new_dur;

    // モグラ消滅（時間切れ）
    if (mogura_led_on && timer_elapsed32(mogura_led_on_time) >= mogura_duration_ms) {
        kb_led_set(mogura_target, false);
        mogura_led_on = false;
        play_mogura_miss();
    }

    // 次のモグラ出現（mogura_next_spawn は出現すべき時刻の絶対値）
    // timer_elapsed32(t) は「t から現在まで何ms経ったか」を返す
    // t が未来なら負値(符号なし大値)になるので < 0x80000000 で「過去かどうか」を判定
    if (!mogura_led_on && timer_elapsed32(mogura_next_spawn) < 0x80000000UL) {
        mogura_spawn();
    }

    // キー入力チェック
    bool keys[8];
    scan_keys(keys);
    if (mogura_led_on && mogura_target >= 0) {
        if (keys[(uint8_t)mogura_target]) {
            // ヒット
            play_mogura_hit();
            mogura_score++;
            kb_led_set(mogura_target, false);
            mogura_led_on = false;
            mogura_next_spawn = timer_read32() + 200;
        }
    }
}

// ============================================================
// モード切り替えタイマー
// ============================================================
static uint32_t mode_switch_timer = 0;
static bool     mode_switch_held  = false;

// ============================================================
// QMK フック：keyboard_post_init_user
// ============================================================
void keyboard_post_init_user(void) {
    for (int i = 0; i < 8; i++) setPinOutput(RED_LEDS[i]);
    setPinOutput(GREEN_PIN);
    setPinOutput(SPEAK_PIN);
    leds_clear();
    srand(timer_read32()); // 疑似乱数初期化
}

// ============================================================
// QMK フック：matrix_scan_user（メインループ）
// ============================================================
void matrix_scan_user(void) {
    bool keys[8];
    scan_keys(keys);

    // ----------------------------------------------------------
    // モード切り替え検出（キー0+1+4+5を2秒長押し）
    // キーマトリクス：0=row0col0, 1=row0col1, 4=row1col0, 5=row1col1
    // ----------------------------------------------------------
    bool mode_combo = keys[0] && keys[1] && keys[4] && keys[5]
                   && !keys[2] && !keys[3] && !keys[6] && !keys[7];

    if (mode_combo) {
        if (!mode_switch_held) {
            mode_switch_timer = timer_read32();
            mode_switch_held  = true;
        }
        if (timer_elapsed32(mode_switch_timer) >= 2000) {
            // モード切り替え実行
            current_mode = (current_mode == MODE_GAME) ? MODE_INPUT : MODE_GAME;
            current_game = GAME_NONE;
            leds_clear();
            play_mode_switch();

            if (current_mode == MODE_GAME) {
                game_launch_lock = true; // キーが離されるまでゲーム起動を禁止
            }
            mode_switch_held = false;
            wait_ms(500); // チャタリング防止
        }
        return;
    } else {
        mode_switch_held = false;
    }

    // ロック解除（起動コンビのキーが全部離れたら）
    if (game_launch_lock) {
        if (!keys[0] && !keys[1] && !keys[4] && !keys[5]) {
            game_launch_lock = false;
        }
        return;
    }

    // ----------------------------------------------------------
    // 入力モード：QMK 通常処理に任せる（process_record_user を使用）
    // ----------------------------------------------------------
    if (current_mode == MODE_INPUT) {
        // 緑LED = レイヤーインジケーター（layer_state_set_user で制御）
        return;
    }

    // ----------------------------------------------------------
    // ゲームモード：終了コンビ（2+3+6+7）チェック
    // ----------------------------------------------------------
    if (current_game != GAME_NONE && check_exit_combo(keys)) {
        current_game = GAME_NONE;
        leds_clear();
        play_tone_blocking(200, 200);
        wait_ms(300);
        return;
    }

    // ----------------------------------------------------------
    // ゲーム選択（キー0+1, 0+2, 0+3）
    // ----------------------------------------------------------
    if (current_game == GAME_NONE) {
        // ゲーム未選択時：押しているキーに対応するLEDを点灯
        for (int i = 0; i < 8; i++) kb_led_set(i, keys[i]);

        if (keys[0] && keys[1] && !keys[2] && !keys[3]
         && !keys[4] && !keys[5] && !keys[6] && !keys[7]) {
            current_game = GAME_ORGAN;
            leds_clear();
            play_game_start();
            init_organ();
            game_launch_lock = true;
        } else if (keys[0] && keys[2] && !keys[1] && !keys[3]
                && !keys[4] && !keys[5] && !keys[6] && !keys[7]) {
            current_game = GAME_MEOSHI;
            leds_clear();
            play_game_start();
            init_meoshi();
            game_launch_lock = true;
        } else if (keys[0] && keys[3] && !keys[1] && !keys[2]
                && !keys[4] && !keys[5] && !keys[6] && !keys[7]) {
            current_game = GAME_MOGURA;
            leds_clear();
            play_game_start();
            init_mogura();
            game_launch_lock = true;
        }
        return;
    }

    // ----------------------------------------------------------
    // ゲーム実行
    // ----------------------------------------------------------
    switch (current_game) {
        case GAME_ORGAN:  run_organ();  break;
        case GAME_MEOSHI: run_meoshi(); break;
        case GAME_MOGURA:
            run_mogura();
            // モグラが終了したらゲーム選択に戻す
            if (!mogura_active) current_game = GAME_NONE;
            break;
        default: break;
    }
}

// ============================================================
// QMK フック：process_record_user
// ゲームモード中はキーボード入力をブロック
// 入力モード中はLED制御付きで通常処理
// ============================================================
bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (current_mode == MODE_GAME) return false; // ゲーム中はHID送信しない

    // 入力モード：押下/リリースに合わせてLED点灯
    uint8_t idx = record->event.key.row * 4 + record->event.key.col;
    if (idx < 8) kb_led_set(idx, record->event.pressed);

    return true;
}

// ============================================================
// QMK フック：layer_state_set_user
// 入力モード時のみ緑LEDでレイヤーを表示
// ============================================================
layer_state_t layer_state_set_user(layer_state_t state) {
    if (current_mode == MODE_INPUT) {
        writePin(GREEN_PIN, get_highest_layer(state) > 0);
    }
    return state;
}

// ============================================================
// キーマップ（入力モード用）
// ============================================================
const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [_BASE]   = LAYOUT(KC_W, KC_A,   KC_S,               LT(_LAYER3, KC_D),
                       KC_SPC, KC_LSFT, LT(_LAYER2, KC_E), LT(_LAYER1, KC_1)),
    [_LAYER1] = LAYOUT(KC_2, KC_3, KC_4, KC_5,
                       KC_6, KC_7, KC_8, KC_9),
    [_LAYER2] = LAYOUT(KC_Q, KC_R, KC_F, KC_TAB,
                       KC_ENT, KC_ESC, KC_T, KC_C),
    [_LAYER3] = LAYOUT(KC_LEFT, KC_UP, KC_DOWN, KC_RIGHT,
                       KC_0,    KC_1,  KC_2,    KC_SPC),
};
