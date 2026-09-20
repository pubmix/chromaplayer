#include "reminders.h"

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static Reminder reminders[REMINDER_MAX];
static size_t count;
static volatile bool alert_pending;
static Reminder pending;
static int last_alert_yday = -1;
static int last_alert_minute = -1;

static void save(void)
{
    nvs_handle_t handle;
    if (nvs_open("chromaplayer", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, "count", (uint8_t)count);
    nvs_set_blob(handle, "items", reminders, sizeof(reminders));
    nvs_commit(handle);
    nvs_close(handle);
}

static void reminder_task(void *arg)
{
    (void)arg;
    for (;;) {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        if (local.tm_year >= 123) {
            for (size_t i = 0; i < count; ++i) {
                if (reminders[i].enabled && reminders[i].hour == local.tm_hour &&
                    reminders[i].minute == local.tm_min &&
                    (last_alert_yday != local.tm_yday || last_alert_minute != local.tm_min)) {
                    pending = reminders[i];
                    alert_pending = true;
                    last_alert_yday = local.tm_yday;
                    last_alert_minute = local.tm_min;
                    break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void Reminders_Initialize(void)
{
    nvs_handle_t handle;
    if (nvs_open("chromaplayer", NVS_READONLY, &handle) == ESP_OK) {
        uint8_t stored = 0;
        size_t size = sizeof(reminders);
        if (nvs_get_u8(handle, "count", &stored) == ESP_OK &&
            nvs_get_blob(handle, "items", reminders, &size) == ESP_OK) {
            count = stored <= REMINDER_MAX ? stored : REMINDER_MAX;
        }
        nvs_close(handle);
    }
    xTaskCreate(reminder_task, "reminders", 2048, NULL, 2, NULL);
}

size_t Reminders_Count(void) { return count; }
const Reminder *Reminders_Get(size_t i) { return i < count ? &reminders[i] : NULL; }

bool Reminders_Add(uint8_t hour, uint8_t minute)
{
    if (count >= REMINDER_MAX || hour > 23 || minute > 59) return false;
    reminders[count++] = (Reminder){ .hour = hour, .minute = minute, .enabled = true };
    save();
    return true;
}

void Reminders_Toggle(size_t i)
{
    if (i < count) { reminders[i].enabled = !reminders[i].enabled; save(); }
}

void Reminders_Delete(size_t i)
{
    if (i >= count) return;
    memmove(&reminders[i], &reminders[i + 1], (count - i - 1) * sizeof(reminders[0]));
    --count;
    save();
}

bool Reminders_TakeAlert(Reminder *out)
{
    if (!alert_pending) return false;
    if (out) *out = pending;
    alert_pending = false;
    return true;
}
