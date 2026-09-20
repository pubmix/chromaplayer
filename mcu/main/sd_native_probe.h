// Read-only, slow native SD probe. Never issues block-write or erase commands.
static bool sd_probe_capture;
static unsigned sd_probe_low[4];
static const gpio_num_t sd_probe_data[] = { PIN_NUM_SD_D0, PIN_NUM_SD_D1, PIN_NUM_SD_D2, PIN_NUM_SD_D3 };
static int sd_probe_clock(void)
{
    gpio_set_level(PIN_NUM_SD_CLK, 1);
    esp_rom_delay_us(5);
    int bit = gpio_get_level(PIN_NUM_SD_CMD);
    if (sd_probe_capture) for (int i = 0; i < 4; ++i)
        if (!gpio_get_level(sd_probe_data[i])) ++sd_probe_low[i];
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
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pins[i], GPIO_PULLUP_ONLY);
    }
    gpio_set_level(PIN_NUM_SD_CLK, 0);
    gpio_set_direction(PIN_NUM_SD_CLK, GPIO_MODE_OUTPUT);
    sd_probe_capture = false;
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
    return 0;
}
