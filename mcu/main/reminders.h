#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REMINDER_MAX 8

typedef struct {
    uint8_t hour;
    uint8_t minute;
    bool enabled;
} Reminder;

void Reminders_Initialize(void);
size_t Reminders_Count(void);
const Reminder *Reminders_Get(size_t index);
bool Reminders_Add(uint8_t hour, uint8_t minute);
void Reminders_Toggle(size_t index);
void Reminders_Delete(size_t index);
bool Reminders_TakeAlert(Reminder *out);

