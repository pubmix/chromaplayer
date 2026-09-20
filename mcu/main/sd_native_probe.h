#include "driver/rtc_io.h"
// Read-only, slow native SD probe. Never issues block-write or erase commands.
static bool sd_probe_capture;
static bool sd_probe_rtc;
static unsigned sd_probe_low[4];
static uint8_t sd_probe_block[514];
static int sd_probe_bit;
static bool sd_probe_started;
static bool sd_probe_four, sd_probe_four_started;
static unsigned sd_probe_four_bit, sd_probe_start_mask;
static uint8_t sd_probe_lanes[4][130];
static void sd_probe_begin_four(void)
{
    sd_probe_four = true; sd_probe_four_started = false; sd_probe_four_bit = 0;
    memset(sd_probe_lanes, 0, sizeof(sd_probe_lanes));
}
static void sd_probe_report_crc(void)
{
    for (unsigned lane = 0; lane < 4; ++lane) {
        uint16_t crc = 0;
        for (unsigned i = 0; i < 128; ++i) {
            crc ^= (uint16_t)sd_probe_lanes[lane][i] << 8;
            for (int b = 0; b < 8; ++b) crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
        }
        uint16_t actual = ((uint16_t)sd_probe_lanes[lane][128] << 8) | sd_probe_lanes[lane][129];
        MUSIC_DIAG("I", "SD_RAW LANE%u bits=%u start=%x crc=%04x/%04x %s", lane,
            sd_probe_four_bit, sd_probe_start_mask, crc, actual,
            sd_probe_four_bit == 1040 && crc == actual ? "VALID" : "INVALID");
    }
    sd_probe_four = false;
}
static const gpio_num_t sd_probe_data[] = { PIN_NUM_SD_D0, PIN_NUM_SD_D1, PIN_NUM_SD_D2, PIN_NUM_SD_D3 };
static int sd_probe_clock(void)
{
    gpio_set_level(PIN_NUM_SD_CLK, 1);
    esp_rom_delay_us(5);
    int bit = gpio_get_level(PIN_NUM_SD_CMD);
    if (sd_probe_capture) for (int i = 0; i < 4; ++i)
        if (!(i == 0 && sd_probe_rtc ? rtc_gpio_get_level(PIN_NUM_SD_D0) : gpio_get_level(sd_probe_data[i]))) ++sd_probe_low[i];
    if (sd_probe_capture) {
        int data = sd_probe_rtc ? rtc_gpio_get_level(PIN_NUM_SD_D0) : gpio_get_level(PIN_NUM_SD_D0);
        if (!sd_probe_started) { if (!data) sd_probe_started = true; }
        else if (sd_probe_bit < (int)sizeof(sd_probe_block) * 8) {
            sd_probe_block[sd_probe_bit / 8] |= data << (7 - sd_probe_bit % 8);
            ++sd_probe_bit;
        }
    }
    if (sd_probe_capture && sd_probe_four) {
        unsigned nibble = 0;
        for (unsigned lane = 0; lane < 4; ++lane)
            nibble |= (lane == 0 && sd_probe_rtc ? rtc_gpio_get_level(PIN_NUM_SD_D0) : gpio_get_level(sd_probe_data[lane])) << lane;
        if (!sd_probe_four_started) {
            if (!(nibble & 0xe)) { sd_probe_four_started = true; sd_probe_start_mask = nibble; }
        } else if (sd_probe_four_bit < 1040) {
            for (unsigned lane = 0; lane < 4; ++lane)
                sd_probe_lanes[lane][sd_probe_four_bit / 8] |= ((nibble >> lane) & 1) << (7 - sd_probe_four_bit % 8);
            ++sd_probe_four_bit;
        }
    }
    gpio_set_level(PIN_NUM_SD_CLK, 0);
    esp_rom_delay_us(5);
    return bit;
}
static bool sd_probe_command(unsigned op, uint32_t arg, uint8_t *response, unsigned bits)
{
    uint8_t packet[6] = { 0x40 | op, arg >> 24, arg >> 16, arg >> 8, arg, 0 };
    uint8_t crc = 0;
    for (int i = 0; i < 5; ++i) for (int b = 7; b >= 0; --b) {
        unsigned feedback = ((crc >> 6) ^ (packet[i] >> b)) & 1;
        crc = (crc << 1) & 0x7f;
        if (feedback) crc ^= 0x09;
    }
    packet[5] = (crc << 1) | 1;
    gpio_set_level(PIN_NUM_SD_CMD, 1);
    gpio_set_direction(PIN_NUM_SD_CMD, GPIO_MODE_INPUT_OUTPUT);
    for (int i = 0; i < 8; ++i) sd_probe_clock();
    for (int i = 0; i < 6; ++i) for (int b = 7; b >= 0; --b) {
        gpio_set_level(PIN_NUM_SD_CMD, (packet[i] >> b) & 1);
        sd_probe_clock();
    }
    gpio_set_direction(PIN_NUM_SD_CMD, GPIO_MODE_INPUT);
    if (!bits) return true;
    memset(response, 0, (bits + 7) / 8);
    unsigned clocks = 0;
    while (sd_probe_clock()) if (++clocks > 1024) {
        MUSIC_DIAG("W", "SD_RAW CMD%u no response", op); return false;
    }
    for (unsigned i = 1; i < bits; ++i)
        response[i / 8] |= sd_probe_clock() << (7 - i % 8);
    MUSIC_DIAG("I", "SD_RAW CMD%u response=%02x%02x%02x%02x%02x%02x", op,
        response[0], response[1], response[2], response[3], response[4], response[5]);
    return true;
}
static int sd_native_probe(void)
{
    gpio_num_t pins[] = { PIN_NUM_SD_CLK, PIN_NUM_SD_CMD, PIN_NUM_SD_D0, PIN_NUM_SD_D1, PIN_NUM_SD_D2, PIN_NUM_SD_D3 };
    for (unsigned i = 0; i < 6; ++i) {
        gpio_hold_dis(pins[i]);
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pins[i], GPIO_PULLUP_ONLY);
    }
    gpio_set_level(PIN_NUM_SD_CLK, 0);
    gpio_set_direction(PIN_NUM_SD_CLK, GPIO_MODE_OUTPUT);
    sd_probe_capture = false; sd_probe_rtc = false; sd_probe_four = false;
    for (int i = 0; i < 100; ++i) sd_probe_clock();
    uint8_t response[17];
    sd_probe_command(0, 0, response, 0);
    if (!sd_probe_command(8, 0x1aa, response, 48)) return 1;
    bool ready = false;
    for (int i = 0; i < 100; ++i) {
        if (!sd_probe_command(55, 0, response, 48) || !sd_probe_command(41, 0x40ff8000, response, 48)) return 1;
        if (response[1] & 0x80) { ready = true; break; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!ready || !sd_probe_command(2, 0, response, 136) || !sd_probe_command(3, 0, response, 48)) return 1;
    uint32_t rca = ((uint32_t)response[1] << 24) | ((uint32_t)response[2] << 16);
    if (!sd_probe_command(7, rca, response, 48)) return 1;
    for (int i = 0; i < 100; ++i) sd_probe_clock();
    if (!sd_probe_command(55, rca, response, 48)) return 1;
    memset(sd_probe_low, 0, sizeof(sd_probe_low));
    sd_probe_capture = true;
    bool replied = sd_probe_command(51, 0, response, 48);
    for (int i = 0; i < 2048; ++i) sd_probe_clock();
    sd_probe_capture = false;
    MUSIC_DIAG("I", "SD_RAW SCR response=%d data_low_counts=%u,%u,%u,%u", replied,
        sd_probe_low[0], sd_probe_low[1], sd_probe_low[2], sd_probe_low[3]);
    // Read the actual first sector, independently of SCR parsing or FatFS.
    if (!sd_probe_command(13, rca, response, 48)) return 1;
    memset(sd_probe_low, 0, sizeof(sd_probe_low));
    memset(sd_probe_block, 0, sizeof(sd_probe_block));
    sd_probe_started = false; sd_probe_bit = 0; sd_probe_capture = true;
    replied = sd_probe_command(17, 0, response, 48);
    for (int i = 0; i < 40000 && sd_probe_bit < (int)sizeof(sd_probe_block) * 8; ++i) {
        sd_probe_clock();
        if (i % 512 == 0) vTaskDelay(1);
    }
    sd_probe_capture = false;
    uint16_t crc = 0;
    for (unsigned i = 0; i < 512; ++i) {
        crc ^= (uint16_t)sd_probe_block[i] << 8;
        for (int b = 0; b < 8; ++b) crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
    }
    uint16_t actual_crc = ((uint16_t)sd_probe_block[512] << 8) | sd_probe_block[513];
    MUSIC_DIAG("I", "SD_RAW SECTOR0 response=%d bits=%d low=%u,%u,%u,%u crc=%04x/%04x signature=%02x%02x", replied,
        sd_probe_bit, sd_probe_low[0], sd_probe_low[1], sd_probe_low[2], sd_probe_low[3], crc, actual_crc,
        sd_probe_block[510], sd_probe_block[511]);
    sd_probe_command(13, rca, response, 48);
    // Exercise every board data wire using native 4-bit mode, then restore 1-bit.
    if (sd_probe_command(55, rca, response, 48) && sd_probe_command(6, 2, response, 48)) {
        memset(sd_probe_low, 0, sizeof(sd_probe_low));
        sd_probe_started = false; sd_probe_bit = 0; sd_probe_capture = true;
        sd_probe_begin_four();
        replied = sd_probe_command(17, 0, response, 48);
        for (int i = 0; i < 40000; ++i) {
            sd_probe_clock();
            if (i % 512 == 0) vTaskDelay(1);
        }
        sd_probe_capture = false;
        MUSIC_DIAG("I", "SD_RAW FOUR_BIT response=%d low=%u,%u,%u,%u", replied,
            sd_probe_low[0], sd_probe_low[1], sd_probe_low[2], sd_probe_low[3]);
        sd_probe_report_crc();
        // Independent RTC input path on the same pad rules out the digital mux.
        rtc_gpio_init(PIN_NUM_SD_D0);
        rtc_gpio_set_direction(PIN_NUM_SD_D0, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_en(PIN_NUM_SD_D0);
        rtc_gpio_pulldown_dis(PIN_NUM_SD_D0);
        sd_probe_rtc = true;
        memset(sd_probe_low, 0, sizeof(sd_probe_low));
        sd_probe_bit = 0; sd_probe_started = false; sd_probe_capture = true;
        sd_probe_begin_four();
        replied = sd_probe_command(17, 0, response, 48);
        for (int i = 0; i < 40000; ++i) {
            sd_probe_clock();
            if (i % 512 == 0) vTaskDelay(1);
        }
        sd_probe_capture = false;
        MUSIC_DIAG("I", "SD_RAW RTC_D0 response=%d low=%u,%u,%u,%u", replied,
            sd_probe_low[0], sd_probe_low[1], sd_probe_low[2], sd_probe_low[3]);
        sd_probe_report_crc();
        sd_probe_rtc = false;
        rtc_gpio_deinit(PIN_NUM_SD_D0);
        gpio_set_direction(PIN_NUM_SD_D0, GPIO_MODE_INPUT);
        gpio_set_pull_mode(PIN_NUM_SD_D0, GPIO_PULLUP_ONLY);
        sd_probe_command(55, rca, response, 48);
        sd_probe_command(6, 0, response, 48);
    }
    return 0;
}
