#include "music_player.h"

#include <stdbool.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "argtable3/argtable3.h"
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "esp_wifi.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "sdmmc_cmd.h"

#include "board.h"
#include "silent.h"
#include "osd.h"
#include "fpga_tx.h"
#include "fpga_rx.h"
#include "connectivity.h"
#include "minimp3_ex.h"
#include "audio_fixture.h"

#define MUSIC_MOUNT_POINT "/sdcard"
#define MUSIC_I2S_PORT I2S_NUM_0
#define MUSIC_INPUT_BYTES 8192
#define MUSIC_INDEX_BYTES 8192

#define MUSIC_DIAG(level, fmt, ...) printf("music " level ": " fmt "\n", ##__VA_ARGS__)
static TaskHandle_t s_player_task;
static char s_path[256];
static volatile bool s_stop_requested;
static bool s_sd_mounted;
static sdmmc_card_t *s_card;
static bool s_sd_spi;
static bool s_probe_spi;
static volatile MusicScanState_t s_scan_state = kMusicScan_Idle;
static volatile unsigned s_scan_progress;
static TaskHandle_t s_scan_task;
static char s_sd_status[64] = "Press A to check SD";
static volatile MusicState_t s_state = kMusicState_Stopped;
static int s_current_index = -1;
static size_t s_track_count;
static char *s_track_paths[MUSIC_MAX_TRACKS];
static char *s_index;
static size_t s_index_used;
static bool s_index_full;
static size_t s_file_count;
static char *s_file_names[MUSIC_MAX_FILES];

typedef struct {
    const char *name;
    const char *url;
} RadioStation;

static const RadioStation s_stations[] = {
    { "Groove Salad", "http://ice5.somafm.com/groovesalad-128-mp3" },
    { "Drone Zone", "http://ice5.somafm.com/dronezone-128-mp3" },
    { "GS Classic", "http://ice5.somafm.com/gsclassic-128-mp3" },
};
static TaskHandle_t s_radio_task;
static volatile bool s_radio_stop;
static volatile int s_radio_station = -1;
static volatile RadioState_t s_radio_state = kRadio_Stopped;
static char s_radio_status[48] = "Select a station";

static void radio_set_status(RadioState_t state, const char *status)
{
    s_radio_state = state;
    strlcpy(s_radio_status, status, sizeof(s_radio_status));
}

static bool s_radio_output;
static bool s_radio_diagnostic;
typedef struct {
    esp_http_client_handle_t client;
    StreamBufferHandle_t bytes;
    volatile bool stop;
    volatile bool done;
} RadioInput;

static void radio_input_task(void *arg)
{
    RadioInput *input = arg;
    uint8_t chunk[1024];
    TickType_t last_data = xTaskGetTickCount();
    while (!input->stop) {
        int got = esp_http_client_read(input->client, (char *)chunk, sizeof(chunk));
        if (got == -ESP_ERR_HTTP_EAGAIN) {
            if (xTaskGetTickCount() - last_data > pdMS_TO_TICKS(10000)) break;
            vTaskDelay(1);
            continue;
        }
        if (got <= 0) break;
        last_data = xTaskGetTickCount();
        size_t sent = 0;
        while (sent < (size_t)got && !input->stop)
            sent += xStreamBufferSend(input->bytes, chunk + sent, got - sent, pdMS_TO_TICKS(50));
    }
    input->done = true;
    vTaskDelete(NULL);
}

static void radio_task(void *unused)
{
    (void)unused;
    uint8_t *input = malloc(MUSIC_INPUT_BYTES);
    mp3dec_t *decoder = malloc(sizeof(*decoder));
    mp3d_sample_t *pcm = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(*pcm));
    MUSIC_DIAG("I", "Radio heap=%lu largest=%u", (unsigned long)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (!input || !pcm || !decoder) {
        radio_set_status(kRadio_Error, "Not enough memory");
        goto finished;
    }

    while (!s_radio_stop) {
        const int station = s_radio_station;
        if (station < 0 || station >= (int)(sizeof(s_stations) / sizeof(s_stations[0]))) break;
        if (s_radio_output) {
            i2s_zero_dma_buffer(MUSIC_I2S_PORT);
            gpio_set_level(PIN_NUM_AUDIO_ROUTE, 1);
        }
        radio_set_status(kRadio_Connecting, "Connecting...");
        esp_http_client_config_t config = {
            .url = s_stations[station].url,
            .timeout_ms = 5000,
            .buffer_size = 4096,
            .keep_alive_enable = true,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client) esp_http_client_set_header(client, "User-Agent", "ChromaPlayer/0.1");
        esp_err_t open_err = client ? esp_http_client_open(client, 0) : ESP_ERR_NO_MEM;
        if (!client || open_err != ESP_OK) {
            MUSIC_DIAG("E", "Radio open failed: %s, free heap %lu",
                     esp_err_to_name(open_err), (unsigned long)esp_get_free_heap_size());
            if (client) esp_http_client_cleanup(client);
            if (s_radio_diagnostic) {
                radio_set_status(kRadio_Error, "Stream connection failed");
                break;
            }
            radio_set_status(kRadio_Connecting, "Reconnecting...");
            for (int i = 0; i < 20 && !s_radio_stop && station == s_radio_station; ++i) vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int64_t header_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        MUSIC_DIAG("I", "Radio headers: status=%d length=%lld", status,
                 (long long)header_length);
        if (header_length < 0 || status < 200 || status >= 300) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            snprintf(s_radio_status, sizeof(s_radio_status), "Station HTTP %d", status);
            s_radio_state = kRadio_Error;
            break;
        }

        esp_http_client_set_timeout_ms(client, 1000);
        RadioInput source = { .client = client };
        source.bytes = xStreamBufferCreate(8192, 1);
        if (!source.bytes || xTaskCreate(radio_input_task, "radio_http", 4096, &source, 4, NULL) != pdPASS) {
            if (source.bytes) vStreamBufferDelete(source.bytes);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            radio_set_status(kRadio_Error, "Not enough buffer memory");
            break;
        }
        radio_set_status(kRadio_Buffering, "Buffering...");
        TickType_t prebuffer = xTaskGetTickCount();
        while (!source.done && !s_radio_stop && station == s_radio_station &&
               xStreamBufferBytesAvailable(source.bytes) < 6144 &&
               xTaskGetTickCount() - prebuffer < pdMS_TO_TICKS(4000)) vTaskDelay(1);
        if (s_radio_output && s_radio_diagnostic) {
            SettingValue_t mute = { .eType = kSettingDataType_U8, .U8 = 1 };
            SilentMode_ApplySetting(&mute);
            FPGA_Tx_SendSysCtl();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        int output_rate = 0;
        mp3dec_init(decoder);
        size_t have = 0;
        bool need_more = true;
        unsigned total_frames = 0, total_bytes = 0;
        TickType_t started = xTaskGetTickCount();
        unsigned valid_frames = 0;
        i2s_zero_dma_buffer(MUSIC_I2S_PORT);
        while (!s_radio_stop && station == s_radio_station) {
            if (s_radio_diagnostic && xTaskGetTickCount() - started > pdMS_TO_TICKS(20000)) break;
            if (need_more || have < 4096) {
                int capacity = MUSIC_INPUT_BYTES - have;
                int got = xStreamBufferReceive(source.bytes, input + have, capacity < 1024 ? capacity : 1024, pdMS_TO_TICKS(50));
                if (got == 0) {
                    if (source.done) break;
                    continue;
                }
                have += (size_t)got;
                total_bytes += (unsigned)got;
                need_more = false;
            }
            mp3dec_frame_info_t info = {0};
            int samples = mp3dec_decode_frame(decoder, input, (int)have, pcm, &info);
            if (!info.frame_bytes) {
                if (have == MUSIC_INPUT_BYTES) {
                    memmove(input, input + 1, --have);
                }
                need_more = true;
                vTaskDelay(1);
                continue;
            }
            size_t consumed = (size_t)info.frame_bytes;
            if (consumed < have) memmove(input, input + consumed, have - consumed);
            have -= consumed;
            if (samples <= 0 || info.hz < 8000 || info.hz > 48000 ||
                (info.channels != 1 && info.channels != 2)) {
                valid_frames = 0;
                continue;
            }
            ++valid_frames;
            ++total_frames;
            int peak = 0;
            for (int i = 0; i < samples * info.channels; ++i) {
                int value = pcm[i];
                if (value < 0) value = -value;
                if (value > peak) peak = value;
            }
            if (total_frames == 1 || total_frames % 500 == 0)
                MUSIC_DIAG("I", "RADIO_DECODE station=%d frames=%u bytes=%u hz=%d channels=%d peak=%d stack_free=%u", station, total_frames, total_bytes, info.hz, info.channels, peak, (unsigned)uxTaskGetStackHighWaterMark(NULL));
            if (s_radio_output && valid_frames >= 3) {
                if (output_rate != info.hz) {
                    gpio_set_level(PIN_NUM_AUDIO_ROUTE, 0);
                    i2s_zero_dma_buffer(MUSIC_I2S_PORT);
                    if (i2s_set_clk(MUSIC_I2S_PORT, info.hz, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO) != ESP_OK) break;
                    output_rate = info.hz;
                }
                // Diagnostics stay very quiet; playback retains headroom.
                const int attenuation = s_radio_diagnostic ? 128 : 16;
                if (info.channels == 1) {
                    for (int i = samples - 1; i >= 0; --i) pcm[i*2] = pcm[i*2+1] = pcm[i] / attenuation;
                } else {
                    for (int i = 0; i < samples*2; ++i) pcm[i] /= attenuation;
                }
                gpio_set_level(PIN_NUM_AUDIO_ROUTE, 1);
                size_t written = 0;
                size_t size = samples * 2 * sizeof(*pcm);
                if (i2s_write(MUSIC_I2S_PORT, pcm, size, &written, pdMS_TO_TICKS(200)) != ESP_OK || written != size) break;
                radio_set_status(s_radio_diagnostic ? kRadio_Buffering : kRadio_Playing,
                                 s_radio_diagnostic ? "USB audio test (muted)" : s_stations[station].name);
            } else {
                radio_set_status(kRadio_Buffering, "Decode test (no output)");
                vTaskDelay(1);
            }

        }
        source.stop = true;
        while (!source.done) vTaskDelay(1);
        vStreamBufferDelete(source.bytes);
        i2s_zero_dma_buffer(MUSIC_I2S_PORT);
        MUSIC_DIAG("I", "RADIO_RESULT station=%d frames=%u bytes=%u stop=%d", station, total_frames, total_bytes, s_radio_stop);
        gpio_set_level(PIN_NUM_AUDIO_ROUTE, 0);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (!s_radio_stop && station == s_radio_station) {
            if (s_radio_diagnostic) {
                radio_set_status(kRadio_Stopped, "Test complete");
                break;
            }
            radio_set_status(kRadio_Connecting, "Reconnecting...");
            for (int i = 0; i < 20 && !s_radio_stop && station == s_radio_station; ++i) vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

finished:
    gpio_set_level(PIN_NUM_AUDIO_ROUTE, 0);
    free(input); free(pcm); free(decoder);
    s_radio_task = NULL;
    s_radio_stop = false;
    if (s_radio_state != kRadio_Error) radio_set_status(kRadio_Stopped, "Stopped");
    vTaskDelete(NULL);
}

static uint8_t raw_spi_byte(uint8_t out)
{
    uint8_t in = 0;
    for (int bit = 7; bit >= 0; --bit) {
        gpio_set_level(PIN_NUM_SD_CLK, 0);
        gpio_set_level(PIN_NUM_SD_CMD, (out >> bit) & 1);
        esp_rom_delay_us(4);
        gpio_set_level(PIN_NUM_SD_CLK, 1);
        in = (uint8_t)((in << 1) | gpio_get_level(PIN_NUM_SD_D0));
        esp_rom_delay_us(4);
    }
    gpio_set_level(PIN_NUM_SD_CLK, 0);
    return in;
}

// Minimal, low-speed electrical probe. A valid card answers CMD0 with R1=01.
// FF means no response reached D0 and points below the filesystem layer.
static uint8_t raw_spi_cmd0(void)
{
    // A failed native mount can leave the SDMMC peripheral attached to these
    // pads. Release it and reset the matrix before bit-banging the probe.
    sdmmc_host_deinit();
    gpio_reset_pin(PIN_NUM_SD_CLK);
    gpio_reset_pin(PIN_NUM_SD_CMD);
    gpio_reset_pin(PIN_NUM_SD_D0);
    gpio_reset_pin(PIN_NUM_SD_D3);
    gpio_config_t outputs = {
        .pin_bit_mask = BIT64(PIN_NUM_SD_CLK) | BIT64(PIN_NUM_SD_CMD) |
                        BIT64(PIN_NUM_SD_D3),
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    gpio_config_t input = {
        .pin_bit_mask = BIT64(PIN_NUM_SD_D0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&outputs);
    gpio_config(&input);
    gpio_set_level(PIN_NUM_SD_D3, 1);
    gpio_set_level(PIN_NUM_SD_CMD, 1);
    for (int i = 0; i < 12; ++i) raw_spi_byte(0xFF);
    gpio_set_level(PIN_NUM_SD_D3, 0);
    raw_spi_byte(0xFF);
    const uint8_t cmd0[] = { 0x40, 0, 0, 0, 0, 0x95 };
    for (size_t i = 0; i < sizeof(cmd0); ++i) raw_spi_byte(cmd0[i]);
    uint8_t response = 0xFF;
    for (int i = 0; i < 16 && response == 0xFF; ++i)
        response = raw_spi_byte(0xFF);
    gpio_set_level(PIN_NUM_SD_D3, 1);
    raw_spi_byte(0xFF);
    return response;
}

static bool has_mp3_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot != NULL && strcasecmp(dot, ".mp3") == 0;
}

static void scan_dir(const char *absolute, const char *relative, int depth)
{
    if (depth > 3 || s_track_count >= MUSIC_MAX_TRACKS) return;
    DIR *dir = opendir(absolute);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_track_count < MUSIC_MAX_TRACKS) {
        if (entry->d_name[0] == '.') continue;
        char abs_child[256];
        char rel_child[MUSIC_MAX_NAME];
        if (strlen(absolute) + 1 + strlen(entry->d_name) >= sizeof(abs_child) ||
            strlen(relative) + (relative[0] ? 1 : 0) + strlen(entry->d_name) >= sizeof(rel_child)) {
            continue;
        }
        strlcpy(abs_child, absolute, sizeof(abs_child));
        strlcat(abs_child, "/", sizeof(abs_child));
        strlcat(abs_child, entry->d_name, sizeof(abs_child));
        strlcpy(rel_child, relative, sizeof(rel_child));
        if (relative[0]) strlcat(rel_child, "/", sizeof(rel_child));
        strlcat(rel_child, entry->d_name, sizeof(rel_child));
        struct stat st;
        if (stat(abs_child, &st) != 0) continue;
        size_t size = strlen(rel_child) + 1 + (S_ISDIR(st.st_mode) ? 1 : 0);
        if (size > MUSIC_INDEX_BYTES - s_index_used) { s_index_full = true; break; }
        char *stored = s_index + s_index_used;
        memcpy(stored, rel_child, strlen(rel_child) + 1);
        if (S_ISDIR(st.st_mode)) strcat(stored, "/");
        s_index_used += size;
        if (s_file_count < MUSIC_MAX_FILES) s_file_names[s_file_count++] = stored;
        if (S_ISDIR(st.st_mode)) {
            scan_dir(abs_child, rel_child, depth + 1);
        } else if (has_mp3_extension(entry->d_name)) {
            s_track_paths[s_track_count++] = stored;
        }
    }
    closedir(dir);
}

static void player_task(void *unused)
{
    const bool diagnostic = unused != NULL;
    SettingValue_t saved_mute = { .eType = kSettingDataType_U8, .U8 = SilentMode_GetState() };
    if (diagnostic) {
        SettingValue_t mute = { .eType = kSettingDataType_U8, .U8 = 1 };
        SilentMode_ApplySetting(&mute);
        FPGA_Tx_SendSysCtl();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    FILE *file = diagnostic ? (FILE *)unused : fopen(s_path, "rb");
    mp3dec_t *decoder = malloc(sizeof(*decoder));
    uint8_t *input = malloc(MUSIC_INPUT_BYTES);
    mp3d_sample_t *pcm = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(*pcm));
    bool failed = !file || !decoder || !input || !pcm;
    unsigned frames = 0;
    if (failed) goto finished;
    mp3dec_init(decoder);
    size_t have = 0;
    bool eof = false, need_more = true;
    int rate = 0;
    i2s_zero_dma_buffer(MUSIC_I2S_PORT);
    while (!s_stop_requested) {
        if (!eof && (need_more || have < 4096)) {
            size_t got = fread(input + have, 1, MUSIC_INPUT_BYTES - have, file);
            have += got;
            if (ferror(file)) { failed = true; break; }
            eof = feof(file);
            need_more = false;
        }
        if (!have) break;
        mp3dec_frame_info_t info = {0};
        int samples = mp3dec_decode_frame(decoder, input, have, pcm, &info);
        if (!info.frame_bytes) {
            if (eof) break;
            if (have == MUSIC_INPUT_BYTES) memmove(input, input + 1, --have);
            need_more = true;
            vTaskDelay(1);
            continue;
        }
        have -= info.frame_bytes;
        memmove(input, input + info.frame_bytes, have);
        if (samples <= 0 || info.hz < 8000 || info.hz > 48000 || (info.channels != 1 && info.channels != 2)) continue;
        if (rate != info.hz) {
            if (i2s_set_clk(MUSIC_I2S_PORT, info.hz, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO) != ESP_OK) { failed = true; break; }
            rate = info.hz;
            MUSIC_DIAG("I", "FILE_FORMAT rate=%d channels=%d", rate, info.channels);
        }
        if (info.channels == 1) {
            for (int i = samples - 1; i >= 0; --i) pcm[i*2] = pcm[i*2+1] = pcm[i] / 16;
        } else {
            for (int i = 0; i < samples*2; ++i) pcm[i] /= 16;
        }
        size_t written = 0, bytes = samples * 2 * sizeof(*pcm);
        gpio_set_level(PIN_NUM_AUDIO_ROUTE, 1);
        if (i2s_write(MUSIC_I2S_PORT, pcm, bytes, &written, pdMS_TO_TICKS(200)) != ESP_OK || written != bytes) { failed = true; break; }
        ++frames;
        s_state = kMusicState_Playing;
    }
finished:
    gpio_set_level(PIN_NUM_AUDIO_ROUTE, 0);
    i2s_zero_dma_buffer(MUSIC_I2S_PORT);
    if (file) fclose(file);
    free(decoder); free(input); free(pcm);
    MUSIC_DIAG("I", "FILE_RESULT frames=%u error=%d stack_free=%u", frames, failed, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (diagnostic) { SilentMode_ApplySetting(&saved_mute); FPGA_Tx_SendSysCtl(); }
    s_state = failed || (!frames && !s_stop_requested) ? kMusicState_Error : kMusicState_Stopped;
    s_stop_requested = false;
    s_player_task = NULL;
    vTaskDelete(NULL);
}

static struct {
    struct arg_str *path;
    struct arg_end *end;
} s_play_args;

static int play_command(int argc, char **argv)
{
    if (!s_sd_mounted || s_radio_task || s_scan_task) return 1;
    if (arg_parse(argc, argv, (void **)&s_play_args) != 0) {
        arg_print_errors(stderr, s_play_args.end, argv[0]);
        return 1;
    }
    if (s_player_task) {
        MUSIC_DIAG("W", "Stop the current track first");
        return 1;
    }
    s_stop_requested = false;
    const char *requested = s_play_args.path->sval[0];
    if (requested[0] == '/') {
        snprintf(s_path, sizeof(s_path), "%s", requested);
    } else {
        snprintf(s_path, sizeof(s_path), MUSIC_MOUNT_POINT "/%s", requested);
    }
    return xTaskCreatePinnedToCore(player_task, "mp3_player", 24576, NULL, 5,
                                   &s_player_task, 0) == pdPASS ? 0 : 1;
}

static int stop_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_player_task) {
        s_stop_requested = true;
    }
    return 0;
}

bool MusicPlayer_IsSDMounted(void) { return s_sd_mounted; }

size_t MusicPlayer_Rescan(void)
{
    if (s_scan_task || s_player_task) return s_track_count;
    s_track_count = 0;
    s_file_count = 0;
    s_index_used = 0;
    s_index_full = false;
    if (s_sd_mounted) scan_dir(MUSIC_MOUNT_POINT, "", 0);
    return s_track_count;
}

static esp_err_t mount_card(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };
    s_card = NULL;
    s_sd_spi = false;
    if (s_probe_spi) {
        // Display owns VSPI/SPI3. Use the independent HSPI/SPI2 host only.
        spi_bus_config_t bus = { .mosi_io_num = PIN_NUM_SD_CMD, .miso_io_num = PIN_NUM_SD_D0,
            .sclk_io_num = PIN_NUM_SD_CLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = 4096 };
        esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
        MUSIC_DIAG("I", "SD SPI2 bus=%s", esp_err_to_name(err));
        if (err != ESP_OK) return err;
        sdmmc_host_t spi_host = SDSPI_HOST_DEFAULT();
        spi_host.slot = SPI2_HOST;
        spi_host.max_freq_khz = 400;
        sdspi_device_config_t spi_slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        spi_slot.host_id = SPI2_HOST;
        spi_slot.gpio_cs = PIN_NUM_SD_D3;
        err = esp_vfs_fat_sdspi_mount(MUSIC_MOUNT_POINT, &spi_host, &spi_slot, &mount, &s_card);
        MUSIC_DIAG("I", "SD SPI2 mount=%s", esp_err_to_name(err));
        if (err != ESP_OK) spi_bus_free(SPI2_HOST);
        else s_sd_spi = true;
        return err;
    }
    gpio_set_pull_mode(PIN_NUM_SD_CMD, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_SD_D0, GPIO_PULLUP_ONLY);
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = 10000;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MUSIC_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        MUSIC_DIAG("W", "Native SD failed: %s; trying SPI", esp_err_to_name(err));
        s_probe_spi = true;
        err = mount_card();
        s_probe_spi = false;
    }
    return err;
}

static void scan_task(void *unused)
{
    (void)unused;
    s_scan_state = kMusicScan_Running;
    s_scan_progress = 10;
    strlcpy(s_sd_status, "Checking card...", sizeof(s_sd_status));
    vTaskDelay(pdMS_TO_TICKS(250));
    if (s_sd_mounted) {
        esp_vfs_fat_sdcard_unmount(MUSIC_MOUNT_POINT, s_card);
        if (s_sd_spi) spi_bus_free(SPI2_HOST);
        s_sd_mounted = false;
        s_card = NULL;
        s_sd_spi = false;
    }
    s_track_count = 0;
    s_file_count = 0;
    s_index_used = 0;
    s_index_full = false;
    s_scan_progress = 35;
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_err_t err = mount_card();
    if (err != ESP_OK) {
        const uint8_t raw = raw_spi_cmd0();
        s_scan_progress = 100;
        s_scan_state = gpio_get_level(PIN_NUM_SD_DETECT) ?
                       kMusicScan_NoCard : kMusicScan_MountError;
        MUSIC_DIAG("W", "%s CD:%d K:%d M:%d D:%d%d%d%d R:%02X",
                 esp_err_to_name(err), gpio_get_level(PIN_NUM_SD_DETECT),
                 gpio_get_level(PIN_NUM_SD_CLK), gpio_get_level(PIN_NUM_SD_CMD),
                 gpio_get_level(PIN_NUM_SD_D0), gpio_get_level(PIN_NUM_SD_D1),
                 gpio_get_level(PIN_NUM_SD_D2), gpio_get_level(PIN_NUM_SD_D3), raw);
        strlcpy(s_sd_status, gpio_get_level(PIN_NUM_SD_DETECT) ? "Insert a microSD card" : "Card not responding", sizeof(s_sd_status));
    } else {
        if (!s_index) s_index = malloc(MUSIC_INDEX_BYTES);
        if (!s_index) {
            esp_vfs_fat_sdcard_unmount(MUSIC_MOUNT_POINT, s_card);
            if (s_sd_spi) spi_bus_free(SPI2_HOST);
            s_card = NULL;
            s_scan_state = kMusicScan_MountError;
            strlcpy(s_sd_status, "No memory for SD index", sizeof(s_sd_status));
            s_scan_task = NULL;
            vTaskDelete(NULL);
            return;
        }
        s_sd_mounted = true;
        s_scan_progress = 65;
        vTaskDelay(pdMS_TO_TICKS(250));
        scan_dir(MUSIC_MOUNT_POINT, "", 0);
        s_scan_progress = 100;
        s_scan_state = kMusicScan_Found;
        snprintf(s_sd_status, sizeof(s_sd_status), "SD found (%s) - %u MP3%s",
                 s_sd_spi ? "SPI" : "SDMMC", (unsigned)s_track_count,
                 s_index_full ? " (index full)" : s_track_count == 1 ? "" : "s");
        MUSIC_DIAG("I", "%s", s_sd_status);
    }
    s_scan_task = NULL;
    vTaskDelete(NULL);
}

bool MusicPlayer_StartScan(void)
{
    if (s_scan_task || s_player_task) return false;
    return xTaskCreatePinnedToCore(scan_task, "sd_scan", 8192, NULL, 4,
                                   &s_scan_task, 0) == pdPASS;
}

MusicScanState_t MusicPlayer_GetScanState(void) { return s_scan_state; }
unsigned MusicPlayer_GetScanProgress(void) { return s_scan_progress; }
const char *MusicPlayer_GetSDStatus(void) { return s_sd_status; }

size_t MusicPlayer_GetTrackCount(void) { return s_track_count; }

const char *MusicPlayer_GetTrackName(size_t index)
{
    if (index >= s_track_count) return NULL;
    const char *name = strrchr(s_track_paths[index], '/');
    return name ? name + 1 : s_track_paths[index];
}

const char *MusicPlayer_GetTrackPath(size_t index)
{
    return index < s_track_count ? s_track_paths[index] : NULL;
}

size_t MusicPlayer_GetFileCount(void) { return s_file_count; }

const char *MusicPlayer_GetFileName(size_t index)
{
    return index < s_file_count ? s_file_names[index] : NULL;
}

int MusicPlayer_PlayIndex(size_t index)
{
    if (!s_sd_mounted || index >= s_track_count || s_player_task || s_radio_task || s_scan_task) return -1;
    s_current_index = (int)index;
    s_stop_requested = false;
    snprintf(s_path, sizeof(s_path), MUSIC_MOUNT_POINT "/%s", s_track_paths[index]);
    return xTaskCreatePinnedToCore(player_task, "mp3_player", 24576, NULL, 5,
                                   &s_player_task, 0) == pdPASS ? 0 : -1;
}

void MusicPlayer_Stop(void)
{
    gpio_set_level(PIN_NUM_AUDIO_ROUTE, 0);
    if (s_player_task) s_stop_requested = true;
}

MusicState_t MusicPlayer_GetState(void) { return s_state; }
int MusicPlayer_GetCurrentIndex(void) { return s_current_index; }

size_t Radio_GetStationCount(void) { return sizeof(s_stations) / sizeof(s_stations[0]); }
const char *Radio_GetStationName(size_t index)
{
    return index < Radio_GetStationCount() ? s_stations[index].name : NULL;
}
int Radio_GetStationIndex(void) { return s_radio_station; }
RadioState_t Radio_GetState(void) { return s_radio_state; }
const char *Radio_GetStatus(void) { return s_radio_status; }

bool Radio_Start(size_t index)
{
    if (index >= Radio_GetStationCount() || s_player_task || s_scan_task || s_radio_stop) return false;
    if (!Connectivity_WifiConnected()) {
        radio_set_status(kRadio_Error, "Connect Wi-Fi first");
        return false;
    }
    if (s_radio_task && s_radio_diagnostic) return false;
    gpio_set_level(PIN_NUM_AUDIO_ROUTE, 0);
    i2s_zero_dma_buffer(MUSIC_I2S_PORT);
    s_radio_station = (int)index;
    if (s_radio_task) {
        radio_set_status(kRadio_Connecting, "Switching station...");
        return true;
    }
    s_radio_stop = false;
    s_radio_output = true;
    s_radio_diagnostic = false;
    radio_set_status(kRadio_Connecting, "Connecting...");
    if (xTaskCreatePinnedToCore(radio_task, "net_radio", 24576, NULL, 5, &s_radio_task, 0) != pdPASS) {
        radio_set_status(kRadio_Error, "Not enough memory");
        return false;
    }
    return true;
}

void Radio_Stop(void)
{
    gpio_set_level(PIN_NUM_AUDIO_ROUTE, 0);
    i2s_zero_dma_buffer(MUSIC_I2S_PORT);
    if (s_radio_task) {
        s_radio_stop = true;
        radio_set_status(kRadio_Stopped, "Stopping...");
    }
}

// Bounded USB-path diagnostic. Codec mute is applied before routing any PCM.
// It remains applied afterwards; this command never enables acoustic output.
static void tone_diagnostic(void *unused)
{
    SettingValue_t mute = { .eType = kSettingDataType_U8, .U8 = 1 };
    SilentMode_ApplySetting(&mute);
    FPGA_Tx_SendSysCtl();
    vTaskDelay(pdMS_TO_TICKS(500));
    i2s_zero_dma_buffer(MUSIC_I2S_PORT);
    esp_err_t err = i2s_set_clk(MUSIC_I2S_PORT, 48000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    int16_t pcm[96];
    // 1 kHz triangle, peak 192 (below -44 dBFS); same-polarity channels to test host capture processing.
    for (int i = 0; i < 48; ++i) {
        int v = i < 24 ? -192 + i * 16 : 192 - (i - 24) * 16;
        pcm[i*2] = v; pcm[i*2+1] = v;
    }
    MUSIC_DIAG("I", "TONE start codec_mute=1 rate=48000 peak=192 err=%s", esp_err_to_name(err));
    if (err == ESP_OK) {
        gpio_set_level(PIN_NUM_AUDIO_ROUTE, 1);
        for (int i = 0; i < 5000 && !s_stop_requested; ++i) {
            if (i % 1000 == 0) { FPGA_Tx_SendAll(); MUSIC_DIAG("I", "FPGA_AUDIO diag=%08lx", FPGA_Rx_GetAudioDiagnostic()); }
            size_t written = 0;
            if (i2s_write(MUSIC_I2S_PORT, pcm, sizeof(pcm), &written, pdMS_TO_TICKS(100)) != ESP_OK || written != sizeof(pcm)) break;
        }
    }
    i2s_zero_dma_buffer(MUSIC_I2S_PORT);
    gpio_set_level(PIN_NUM_AUDIO_ROUTE, 0);
    MUSIC_DIAG("I", "TONE finished; codec mute retained");
    s_player_task = NULL;
    s_stop_requested = false;
    vTaskDelete(NULL);
}

static int sd_pad_diagnostic(void)
{
    if (s_sd_mounted || s_scan_task || s_player_task) return 1;
    const gpio_num_t pads[] = { PIN_NUM_SD_CMD, PIN_NUM_SD_D0, PIN_NUM_SD_D1, PIN_NUM_SD_D2, PIN_NUM_SD_D3 };
    for (unsigned i = 0; i < sizeof(pads)/sizeof(pads[0]); ++i) {
        gpio_reset_pin(pads[i]);
        gpio_set_direction(pads[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pads[i], GPIO_PULLDOWN_ONLY);
        vTaskDelay(pdMS_TO_TICKS(20));
        int down = gpio_get_level(pads[i]);
        gpio_set_pull_mode(pads[i], GPIO_PULLUP_ONLY);
        vTaskDelay(pdMS_TO_TICKS(20));
        int up = gpio_get_level(pads[i]);
        gpio_set_pull_mode(pads[i], GPIO_FLOATING);
        vTaskDelay(pdMS_TO_TICKS(20));
        MUSIC_DIAG("I", "SD_PAD gpio=%d weak_down=%d weak_up=%d floating=%d", pads[i], down, up, gpio_get_level(pads[i]));
    }
    // Check only the host-driven pins, with no command or filesystem write.
    gpio_set_direction(PIN_NUM_SD_D3, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(PIN_NUM_SD_D3, 1);
    const gpio_num_t driven[] = { PIN_NUM_SD_CMD, PIN_NUM_SD_CLK, PIN_NUM_SD_D3 };
    for (unsigned i = 0; i < sizeof(driven)/sizeof(driven[0]); ++i) {
        gpio_reset_pin(driven[i]);
        gpio_set_direction(driven[i], GPIO_MODE_INPUT_OUTPUT);
        gpio_set_level(driven[i], 0);
        esp_rom_delay_us(10);
        int low = gpio_get_level(driven[i]);
        gpio_set_level(driven[i], 1);
        esp_rom_delay_us(10);
        MUSIC_DIAG("I", "SD_DRIVE gpio=%d low=%d high=%d", driven[i], low, gpio_get_level(driven[i]));
    }
    gpio_set_level(PIN_NUM_SD_CLK, 0);
    MUSIC_DIAG("I", "SD_DETECT=%d", gpio_get_level(PIN_NUM_SD_DETECT));
    return 0;
}

static int diagnostic_command(int argc, char **argv)
{
    if (argc < 2) return 1;
    if (!strcmp(argv[1], "wifidrop")) return esp_wifi_disconnect() == ESP_OK ? 0 : 1;
    if (!strcmp(argv[1], "filetest")) {
        if (s_player_task || s_radio_task || s_scan_task) return 1;
        FILE *fixture = fmemopen((void *)audio_fixture, sizeof(audio_fixture), "rb");
        if (!fixture) return 1;
        s_stop_requested = false;
        if (xTaskCreatePinnedToCore(player_task, "file_test", 24576, fixture, 5, &s_player_task, 0) != pdPASS) { fclose(fixture); return 1; }
        return 0;
    }
    if (!strcmp(argv[1], "play") && argc == 3) return Radio_Start(atoi(argv[2])) ? 0 : 1;
    if (!strcmp(argv[1], "pads")) return sd_pad_diagnostic();
    if (!strcmp(argv[1], "fpga")) { FPGA_Tx_SendAll(); vTaskDelay(pdMS_TO_TICKS(200)); MUSIC_DIAG("I", "FPGA_AUDIO diag=%08lx", FPGA_Rx_GetAudioDiagnostic()); return 0; }
    if (!strcmp(argv[1], "tone")) {
        if (s_player_task || s_radio_task || s_scan_task) return 1;
        s_stop_requested = false;
        return xTaskCreatePinnedToCore(tone_diagnostic, "audio_test", 4096, NULL, 5, &s_player_task, 0) == pdPASS ? 0 : 1;
    }
    if (!strcmp(argv[1], "sd") || !strcmp(argv[1], "sdspi")) {
        if (s_scan_task || s_player_task) return 1;
        s_probe_spi = !strcmp(argv[1], "sdspi");
        return MusicPlayer_StartScan() ? 0 : 1;
    }
    if (!strcmp(argv[1], "wifi")) {
        MUSIC_DIAG("I", "Wi-Fi before retry: %s", Connectivity_WifiStatus());
        MUSIC_DIAG("I", "Wi-Fi connect: %s", esp_err_to_name(esp_wifi_connect()));
        return 0;
    }
    if (!strcmp(argv[1], "stop")) { Radio_Stop(); MusicPlayer_Stop(); return 0; }
    if ((!strcmp(argv[1], "radio") || !strcmp(argv[1], "radioaudio")) && argc == 3) {
        int station = atoi(argv[2]);
        if (station < 0 || station >= Radio_GetStationCount() || s_player_task || s_scan_task) return 1;
        if (!Connectivity_WifiConnected()) { MUSIC_DIAG("E", "No Wi-Fi IP"); return 1; }
        s_radio_station = station;
        if (s_radio_task) return 0;
        s_radio_diagnostic = true;
        s_radio_output = !strcmp(argv[1], "radioaudio");
        s_radio_stop = false;
        return xTaskCreatePinnedToCore(radio_task, "radio_decode", 24576, NULL, 5, &s_radio_task, 0) == pdPASS ? 0 : 1;
    }
    MUSIC_DIAG("I", "OSD visible=%d silent=%d", OSD_IsVisible(), SilentMode_GetState());
    MUSIC_DIAG("I", "SD=%s mounted=%d tracks=%u wifi=%d radio=%s wifi_status=%s", s_sd_status, s_sd_mounted, (unsigned)s_track_count, Connectivity_WifiConnected(), s_radio_status, Connectivity_WifiStatus());
    return 0;
}

void MusicPlayer_Initialize(void)
{
    const i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };
    const i2s_pin_config_t pins = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = PIN_NUM_I2S_BCLK,
        .ws_io_num = PIN_NUM_I2S_WS,
        .data_out_num = PIN_NUM_I2S_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
    ESP_ERROR_CHECK(i2s_driver_install(MUSIC_I2S_PORT, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(MUSIC_I2S_PORT, &pins));

    gpio_config_t route = {
        .pin_bit_mask = BIT64(PIN_NUM_AUDIO_ROUTE),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&route));
    gpio_set_level(PIN_NUM_AUDIO_ROUTE, 0);

    gpio_config_t card_detect = {
        .pin_bit_mask = BIT64(PIN_NUM_SD_DETECT),
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&card_detect));

    // Card initialization can take several seconds on a missing or marginal
    // card. Keep boot/UI deterministic; Files -> A starts the scan task.

    s_play_args.path = arg_str1(NULL, NULL, "PATH", "MP3 path on the SD card");
    s_play_args.end = arg_end(2);
    const esp_console_cmd_t play = {
        .command = "music_play",
        .help = "Play an MP3, e.g. music_play Music/song.mp3",
        .func = play_command,
        .argtable = &s_play_args,
    };
    const esp_console_cmd_t stop = {
        .command = "music_stop",
        .help = "Stop MP3 playback",
        .func = stop_command,
    };
    const esp_console_cmd_t diagnostic = { .command = "music_diag", .help = "Read-only sd, status, radio INDEX (decode only), stop", .func = diagnostic_command };
    ESP_ERROR_CHECK(esp_console_cmd_register(&diagnostic));
    ESP_ERROR_CHECK(esp_console_cmd_register(&play));
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop));
}

