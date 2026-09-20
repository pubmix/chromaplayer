#pragma once

#include <stdbool.h>
#include <stddef.h>

#define MUSIC_MAX_TRACKS 128
#define MUSIC_MAX_NAME   256
#define MUSIC_MAX_FILES  128

typedef enum {
    kMusicState_Stopped,
    kMusicState_Playing,
    kMusicState_Error,
} MusicState_t;

typedef enum {
    kMusicScan_Idle,
    kMusicScan_Running,
    kMusicScan_Found,
    kMusicScan_NoCard,
    kMusicScan_MountError,
} MusicScanState_t;

typedef enum {
    kRadio_Stopped,
    kRadio_Connecting,
    kRadio_Buffering,
    kRadio_Playing,
    kRadio_Error,
} RadioState_t;

// Mounts the built-in microSD slot and registers the `music_play` and
// `music_stop` console commands. MP3 files are played through the private
// FPGA I2S bridge to the Chromatic speaker/headphone codec.
void MusicPlayer_Initialize(void);

bool MusicPlayer_IsSDMounted(void);
size_t MusicPlayer_Rescan(void);
bool MusicPlayer_StartScan(void);
MusicScanState_t MusicPlayer_GetScanState(void);
unsigned MusicPlayer_GetScanProgress(void);
const char *MusicPlayer_GetSDStatus(void);
size_t MusicPlayer_GetTrackCount(void);
const char *MusicPlayer_GetTrackName(size_t index);
const char *MusicPlayer_GetTrackPath(size_t index);
size_t MusicPlayer_GetFileCount(void);
const char *MusicPlayer_GetFileName(size_t index);
int MusicPlayer_PlayIndex(size_t index);
int MusicPlayer_PlayFileIndex(size_t index);
void MusicPlayer_Stop(void);
MusicState_t MusicPlayer_GetState(void);
int MusicPlayer_GetCurrentIndex(void);

size_t Radio_GetStationCount(void);
const char *Radio_GetStationName(size_t index);
int Radio_GetStationIndex(void);
RadioState_t Radio_GetState(void);
const char *Radio_GetStatus(void);
bool Radio_Start(size_t index);
void Radio_Stop(void);
