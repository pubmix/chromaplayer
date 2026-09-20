#include "ipod_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include "button.h"
#include "connectivity.h"
#include "music_player.h"
#include "osd.h"
#include "osd_shared.h"
#include "reminders.h"
#include "brightness.h"
#include "frameblend.h"
#include "color_correct_lcd.h"
#include "color_correct_usb.h"

typedef enum {
    PAGE_HOME,
    PAGE_MUSIC,
    PAGE_RADIO,
    PAGE_NOW_PLAYING,
    PAGE_PLAYLISTS,
    PAGE_FILES,
    PAGE_CLOCK,
    PAGE_REMINDERS,
    PAGE_WIFI,
    PAGE_WIFI_PASSWORD,
    PAGE_BLUETOOTH,
    PAGE_SETTINGS,
    PAGE_ABOUT,
    PAGE_DIAGNOSTICS,
} Page;

static const char *home_items[] = {
    "Music", "Radio", "Now Playing", "Playlists", "Files", "Clock", "Reminders",
    "Wi-Fi", "Bluetooth", "Settings", "About", "Console"
};

static OSD_Widget_t widget;
static lv_obj_t *root;
static lv_obj_t *title_label;
static lv_obj_t *body_label;
static lv_obj_t *footer_label;
static lv_style_t root_style;
static Page page = PAGE_HOME;
static unsigned selection;
static bool dirty = true;
static time_t last_clock;
static char wifi_ssid[33];
static char wifi_password[65];
static unsigned key_row, key_col;
static bool key_upper;
static bool show_password;
static const char *keyboard_rows[] = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm", "^-_.@!" };
static const char *settings_items[] = { "Brightness", "Frame blending", "LCD color", "USB color", "Rescan SD", "Wi-Fi", "Bluetooth", "About" };

static void format_clock(char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    if (local.tm_year < 123) snprintf(out, out_size, "--:--");
    else strftime(out, out_size, "%H:%M", &local);
}

static void set_page(Page next)
{
    page = next;
    selection = 0;
    dirty = true;
}

static void render_list(const char *title, const char *const *items, size_t count)
{
    char clock[8], body[640];
    format_clock(clock, sizeof(clock));
    lv_label_set_text_fmt(title_label, "%-17.17s%s", title, clock);
    body[0] = 0;
    const size_t first = selection > 4 ? selection - 4 : 0;
    const size_t end = count < first + 6 ? count : first + 6;
    for (size_t i = first; i < end; ++i) {
        char line[128];
        snprintf(line, sizeof(line), "%s%s\n", i == selection ? "> " : "  ", items[i]);
        strlcat(body, line, sizeof(body));
    }
    lv_label_set_text(body_label, body[0] ? body : "  (empty)");
    lv_label_set_text(footer_label, "A select     B back");
}

static void render_music(void)
{
    static const char *names[MUSIC_MAX_TRACKS];
    const size_t count = MusicPlayer_GetTrackCount();
    for (size_t i = 0; i < count; ++i) names[i] = MusicPlayer_GetTrackName(i);
    render_list("Music", names, count);
    if (!MusicPlayer_IsSDMounted()) lv_label_set_text(body_label, "No SD card\n\nInsert FAT32 card");
}

static void render_info(const char *title, const char *text, const char *footer)
{
    char clock[8];
    format_clock(clock, sizeof(clock));
    lv_label_set_text_fmt(title_label, "%-17.17s%s", title, clock);
    lv_label_set_text(body_label, text);
    lv_label_set_text(footer_label, footer);
}

static void render_wifi(void)
{
    if (Connectivity_WifiConnecting()) {
        render_info("Wi-Fi", Connectivity_WifiStatus(), "B back");
        return;
    }
    if (Connectivity_WifiScanning()) {
        render_info("Wi-Fi", "Scanning...", "B back");
        return;
    }
    const size_t count = Connectivity_WifiCount();
    if (!count) {
        char text[120];
        snprintf(text, sizeof(text), "%s\n\nPress A to scan", Connectivity_WifiStatus());
        render_info("Wi-Fi", text, "A scan      B back");
        return;
    }
    static char rows[CONNECTIVITY_MAX_WIFI][48];
    static const char *names[CONNECTIVITY_MAX_WIFI];
    for (size_t i = 0; i < count; ++i) {
        const ConnectivityWifiAP *ap = Connectivity_WifiGet(i);
        snprintf(rows[i], sizeof(rows[i]), "%s %s", ap->secured ? "*" : " ", ap->ssid);
        names[i] = rows[i];
    }
    render_list("Wi-Fi", names, count);
}

static void render_password(void)
{
    char masked[65], text[640];
    const size_t length = strlen(wifi_password);
    memset(masked, '*', length);
    masked[length] = 0;
    snprintf(text, sizeof(text), "%.20s\nPass: %.21s\n\n", wifi_ssid,
             show_password ? wifi_password : masked);
    for (size_t row = 0; row < ARRAY_SIZE(keyboard_rows); ++row) {
        for (size_t col = 0; keyboard_rows[row][col]; ++col) {
            char key[5];
            char c = keyboard_rows[row][col];
            if (key_upper && isalpha((unsigned char)c)) c = (char)toupper((unsigned char)c);
            snprintf(key, sizeof(key), row == key_row && col == key_col ? "[%c]" : "%c", c);
            strlcat(text, key, sizeof(text));
        }
        strlcat(text, "\n", sizeof(text));
    }
    render_info("Wi-Fi Password", text, show_password ? "B erase SEL hide" : "B erase SEL show");
}

static void render_radio(void)
{
    static const char *names[8];
    const size_t count = Radio_GetStationCount();
    for (size_t i = 0; i < count; ++i) names[i] = Radio_GetStationName(i);
    render_list("Internet Radio", names, count);
    lv_label_set_text_fmt(footer_label, "A play  %.28s", Radio_GetStatus());
}

static void render_files(void)
{
    char text[160];
    if (MusicPlayer_GetScanState() == kMusicScan_Running) {
        unsigned p = MusicPlayer_GetScanProgress();
        unsigned bars = p / 10;
        char bar[12];
        for (unsigned i = 0; i < 10; ++i) bar[i] = i < bars ? '#' : '-';
        bar[10] = 0;
        snprintf(text, sizeof(text), "Scanning SD...\n\n[%s] %u%%\n\n%s", bar, p,
                 MusicPlayer_GetSDStatus());
        render_info("Files", text, "Please wait");
    } else if (MusicPlayer_IsSDMounted()) {
        static const char *names[MUSIC_MAX_FILES];
        const size_t count = MusicPlayer_GetFileCount();
        for (size_t i = 0; i < count; ++i) names[i] = MusicPlayer_GetFileName(i);
        render_list("Files", names, count);
        lv_label_set_text(footer_label, "Up/Down browse  Start scan");
    } else {
        snprintf(text, sizeof(text), "%s\n\nPress A to check", MusicPlayer_GetSDStatus());
        render_info("Files", text, "A scan  B back");
    }
}

static void render_diagnostics(void)
{
    char text[220];
    snprintf(text, sizeof(text), "SD: %s\nTracks: %u\nWi-Fi: %s\nAudio: %s",
             MusicPlayer_IsSDMounted() ? "mounted" : "missing",
             (unsigned)MusicPlayer_GetTrackCount(), Connectivity_WifiStatus(),
             MusicPlayer_GetState() == kMusicState_Error ? "error" :
             MusicPlayer_GetState() == kMusicState_Playing ? "playing" : "stopped");
    render_info("Console", text, "A scan SD  B back");
}

static void render_bluetooth(void)
{
#if !CONFIG_BT_ENABLED
    render_info("Bluetooth", "Unavailable in this build\n\nESP32 RAM limit with Wi-Fi,\nMP3 and full-screen UI", "B back");
    return;
#else
    if (Connectivity_BluetoothScanning()) { render_info("Bluetooth", "Scanning for headphones...", "B back"); return; }
    if (Connectivity_BluetoothConnected()) {
        char text[96]; snprintf(text, sizeof(text), "Connected\n\n%s", Connectivity_BluetoothName());
        render_info("Bluetooth", text, "B back"); return;
    }
    const size_t count = Connectivity_BluetoothCount();
    if (!count) { render_info("Bluetooth", "Not connected\n\nPress A to scan", "A scan      B back"); return; }
    static const char *names[CONNECTIVITY_MAX_BT];
    for (size_t i = 0; i < count; ++i) names[i] = Connectivity_BluetoothGet(i)->name;
    render_list("Bluetooth", names, count);
#endif
}

static void render_settings(void)
{
    static char rows[4][40];
    static const char *names[ARRAY_SIZE(settings_items)];
    snprintf(rows[0], sizeof(rows[0]), "Brightness: %u", (unsigned)Brightness_GetLevel());
    snprintf(rows[1], sizeof(rows[1]), "Frame blending: %s", FrameBlend_GetState() == kFrameBlendState_On ? "On" : "Off");
    snprintf(rows[2], sizeof(rows[2]), "LCD color: %s", ColorCorrectLCD_GetState() == kColorCorrectLCDState_On ? "On" : "Off");
    snprintf(rows[3], sizeof(rows[3]), "USB color: %s", ColorCorrectUSB_GetState() == kColorCorrectUSBState_On ? "On" : "Off");
    for (size_t i = 0; i < ARRAY_SIZE(settings_items); ++i) names[i] = i < 4 ? rows[i] : settings_items[i];
    render_list("Settings", names, ARRAY_SIZE(settings_items));
}

static void render_reminders(void)
{
    static char rows[REMINDER_MAX][32];
    static const char *names[REMINDER_MAX];
    const size_t count = Reminders_Count();
    for (size_t i = 0; i < count; ++i) {
        const Reminder *r = Reminders_Get(i);
        snprintf(rows[i], sizeof(rows[i]), "%s %02u:%02u", r->enabled ? "ON " : "OFF",
                 (unsigned)r->hour, (unsigned)r->minute);
        names[i] = rows[i];
    }
    render_list("Reminders", names, count);
    lv_label_set_text(footer_label, "A toggle Start +5m Select delete");
}

static OSD_Result_t draw(void *arg)
{
    (void)arg;
    // Gfx_Start() creates the full-screen chroma-key tile after this UI.
    // Keep ChromaPlayer above that tile so it is included in the FPGA overlay.
    lv_obj_move_foreground(root);
    const time_t now = time(NULL);
    if (!dirty && now == last_clock) return kOSD_Result_Ok;
    last_clock = now;
    dirty = false;
    switch (page) {
    case PAGE_HOME: render_list("Chromatic", home_items, ARRAY_SIZE(home_items)); break;
    case PAGE_MUSIC: render_music(); break;
    case PAGE_RADIO: render_radio(); break;
    case PAGE_PLAYLISTS: render_info("Playlists", "On-The-Go\n\nM3U playlists on SD", "A open      B back"); break;
    case PAGE_FILES: render_files(); break;
    case PAGE_NOW_PLAYING: {
        const int index = MusicPlayer_GetCurrentIndex();
        const char *name = index >= 0 ? MusicPlayer_GetTrackName((size_t)index) : NULL;
        char text[180];
        snprintf(text, sizeof(text), "Now Playing\n\n%s\n\n%s", name ? name : "Nothing selected",
                 MusicPlayer_GetState() == kMusicState_Playing ? "Playing" : "Stopped");
        render_info("Player", text, "A play   B stop/back");
        break;
    }
    case PAGE_CLOCK: {
        char text[96]; struct tm local; localtime_r(&now, &local);
        if (local.tm_year < 123) snprintf(text, sizeof(text), "Clock not set\n\nConnect Wi-Fi to sync");
        else strftime(text, sizeof(text), "%H:%M:%S\n\n%A\n%B %d, %Y", &local);
        render_info("Clock", text, "B back"); break;
    }
    case PAGE_REMINDERS: render_reminders(); break;
    case PAGE_WIFI: render_wifi(); break;
    case PAGE_WIFI_PASSWORD: render_password(); break;
    case PAGE_BLUETOOTH: render_bluetooth(); break;
    case PAGE_SETTINGS: render_settings(); break;
    case PAGE_ABOUT: render_info("About", "ChromaPlayer\nPrivate prototype 0.1\n\nFPGA 19.0 / MCU 4.3", "B back"); break;
    case PAGE_DIAGNOSTICS: render_diagnostics(); break;
    }
    return kOSD_Result_Ok;
}

static void home_select(void)
{
    static const Page destinations[] = { PAGE_MUSIC, PAGE_RADIO, PAGE_NOW_PLAYING, PAGE_PLAYLISTS,
        PAGE_FILES, PAGE_CLOCK, PAGE_REMINDERS, PAGE_WIFI, PAGE_BLUETOOTH,
        PAGE_SETTINGS, PAGE_ABOUT, PAGE_DIAGNOSTICS };
    if (selection < ARRAY_SIZE(destinations)) set_page(destinations[selection]);
    else OSD_SetVisiblityState(false);
}

static OSD_Result_t on_button(Button_t button, ButtonState_t state, void *arg)
{
    (void)arg;
    if (state != kButtonState_Pressed) return kOSD_Result_Ok;
    size_t count = page == PAGE_HOME ? ARRAY_SIZE(home_items) :
                   page == PAGE_MUSIC ? MusicPlayer_GetTrackCount() :
                   page == PAGE_RADIO ? Radio_GetStationCount() :
                   page == PAGE_FILES ? MusicPlayer_GetFileCount() :
                   page == PAGE_REMINDERS ? Reminders_Count() :
                   page == PAGE_WIFI ? Connectivity_WifiCount() :
                   page == PAGE_BLUETOOTH ? Connectivity_BluetoothCount() :
                   page == PAGE_SETTINGS ? ARRAY_SIZE(settings_items) : 1;
    if (page == PAGE_WIFI_PASSWORD) {
        size_t row_len = strlen(keyboard_rows[key_row]);
        if (button == kButton_Left) { key_col = (key_col + row_len - 1) % row_len; dirty = true; }
        else if (button == kButton_Right) { key_col = (key_col + 1) % row_len; dirty = true; }
        else if (button == kButton_Up) { key_row = (key_row + ARRAY_SIZE(keyboard_rows) - 1) % ARRAY_SIZE(keyboard_rows); if (key_col >= strlen(keyboard_rows[key_row])) key_col = strlen(keyboard_rows[key_row]) - 1; dirty = true; }
        else if (button == kButton_Down) { key_row = (key_row + 1) % ARRAY_SIZE(keyboard_rows); if (key_col >= strlen(keyboard_rows[key_row])) key_col = strlen(keyboard_rows[key_row]) - 1; dirty = true; }
        else if (button == kButton_A && strlen(wifi_password) < sizeof(wifi_password) - 1) {
            size_t n = strlen(wifi_password);
            char c = keyboard_rows[key_row][key_col];
            if (c == '^') key_upper = !key_upper;
            else { if (key_upper && isalpha((unsigned char)c)) c = (char)toupper((unsigned char)c); wifi_password[n] = c; wifi_password[n + 1] = 0; }
            dirty = true;
        } else if (button == kButton_Select) {
            show_password = !show_password; dirty = true;
        } else if (button == kButton_Start) {
            Connectivity_WifiConnect(wifi_ssid, wifi_password); set_page(PAGE_WIFI);
        } else if (button == kButton_B) {
            if (wifi_password[0]) wifi_password[strlen(wifi_password) - 1] = 0;
            else set_page(PAGE_WIFI);
            dirty = true;
        }
        return kOSD_Result_Ok;
    }
    if (button == kButton_Up && selection > 0) { --selection; dirty = true; }
    else if (button == kButton_Down && selection + 1 < count) { ++selection; dirty = true; }
    else if (button == kButton_B) {
        if (page == PAGE_HOME) OSD_SetVisiblityState(false);
        else if (page == PAGE_NOW_PLAYING) { MusicPlayer_Stop(); set_page(PAGE_MUSIC); }
        else if (page == PAGE_RADIO) { Radio_Stop(); set_page(PAGE_HOME); }
        else set_page(PAGE_HOME);
    } else if (button == kButton_A) {
        if (page == PAGE_HOME) home_select();
        else if (page == PAGE_MUSIC && count) {
            if (MusicPlayer_GetState() == kMusicState_Playing) MusicPlayer_Stop();
            if (MusicPlayer_PlayIndex(selection) == 0) set_page(PAGE_NOW_PLAYING);
        } else if (page == PAGE_RADIO && count) {
            Radio_Start(selection); dirty = true;
        } else if (page == PAGE_FILES) {
            if (!MusicPlayer_IsSDMounted()) MusicPlayer_StartScan();
            dirty = true;
        } else if (page == PAGE_DIAGNOSTICS) { MusicPlayer_StartScan(); dirty = true; }
        else if (page == PAGE_WIFI) {
            if (!Connectivity_WifiCount()) Connectivity_WifiScan();
            else {
                const ConnectivityWifiAP *ap = Connectivity_WifiGet(selection);
                if (ap) {
                    strlcpy(wifi_ssid, ap->ssid, sizeof(wifi_ssid));
                    wifi_password[0] = 0;
                    if (ap->secured) set_page(PAGE_WIFI_PASSWORD);
                    else { Connectivity_WifiConnect(wifi_ssid, ""); dirty = true; }
                }
            }
        } else if (page == PAGE_BLUETOOTH) {
            if (!Connectivity_BluetoothCount()) Connectivity_BluetoothScan();
            else Connectivity_BluetoothConnect(selection);
            dirty = true;
        } else if (page == PAGE_SETTINGS) {
            switch (selection) {
            case 0: Brightness_Update(Brightness_GetLevel() >= 10 ? 1 : Brightness_GetLevel() + 1); break;
            case 1: FrameBlend_Update(FrameBlend_GetState() == kFrameBlendState_On ? kFrameBlendState_Off : kFrameBlendState_On); break;
            case 2: ColorCorrectLCD_Update(ColorCorrectLCD_GetState() == kColorCorrectLCDState_On ? kColorCorrectLCDState_Off : kColorCorrectLCDState_On); break;
            case 3: ColorCorrectUSB_Update(ColorCorrectUSB_GetState() == kColorCorrectUSBState_On ? kColorCorrectUSBState_Off : kColorCorrectUSBState_On); break;
            case 4: MusicPlayer_Rescan(); break;
            case 5: set_page(PAGE_WIFI); break;
            case 6: set_page(PAGE_BLUETOOTH); break;
            case 7: set_page(PAGE_ABOUT); break;
            }
            dirty = true;
        } else if (page == PAGE_REMINDERS && count) {
            Reminders_Toggle(selection); dirty = true;
        }
        else if (page == PAGE_NOW_PLAYING && MusicPlayer_GetState() != kMusicState_Playing &&
                 MusicPlayer_GetCurrentIndex() >= 0) {
            MusicPlayer_PlayIndex((size_t)MusicPlayer_GetCurrentIndex()); dirty = true;
        }
    } else if (page == PAGE_FILES && button == kButton_Start) {
        MusicPlayer_StartScan(); dirty = true;
    } else if (page == PAGE_REMINDERS && button == kButton_Start) {
        time_t now = time(NULL) + 300;
        struct tm local; localtime_r(&now, &local);
        if (local.tm_year >= 123) Reminders_Add((uint8_t)local.tm_hour, (uint8_t)local.tm_min);
        dirty = true;
    } else if (page == PAGE_REMINDERS && button == kButton_Select && count) {
        Reminders_Delete(selection);
        if (selection && selection >= Reminders_Count()) --selection;
        dirty = true;
    }
    return kOSD_Result_Ok;
}

void IPodUI_Init(lv_obj_t *screen)
{
    lv_style_init(&root_style);
    lv_style_set_bg_color(&root_style, lv_color_hex(0xF2F2F2));
    lv_style_set_bg_opa(&root_style, LV_OPA_COVER);
    lv_style_set_border_width(&root_style, 0);
    lv_style_set_pad_all(&root_style, 0);

    root = lv_obj_create(screen);
    lv_obj_set_size(root, 160, 144);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_add_style(root, &root_style, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    title_label = lv_label_create(root);
    lv_obj_set_pos(title_label, 4, 3);
    lv_obj_set_width(title_label, 152);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(title_label, lv_color_black(), 0);

    lv_obj_t *rule = lv_obj_create(root);
    lv_obj_set_pos(rule, 0, 18); lv_obj_set_size(rule, 160, 2);
    lv_obj_set_style_bg_color(rule, lv_color_hex(0x6B6B6B), 0);
    lv_obj_set_style_border_width(rule, 0, 0);

    body_label = lv_label_create(root);
    lv_obj_set_pos(body_label, 7, 25);
    lv_obj_set_size(body_label, 148, 100);
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(body_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(body_label, lv_color_black(), 0);

    footer_label = lv_label_create(root);
    lv_obj_set_pos(footer_label, 4, 129);
    lv_obj_set_width(footer_label, 152);
    lv_obj_set_style_text_font(footer_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(footer_label, lv_color_hex(0x444444), 0);
    lv_label_set_long_mode(footer_label, LV_LABEL_LONG_CLIP);

    widget.Name = "ChromaPlayer";
    widget.fnDraw = draw;
    widget.fnOnButton = on_button;
    OSD_AddWidget(&widget);
    dirty = true;
}
