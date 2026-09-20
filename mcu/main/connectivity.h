#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONNECTIVITY_MAX_WIFI 12
#define CONNECTIVITY_MAX_BT 10

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secured;
} ConnectivityWifiAP;

typedef struct {
    char name[33];
    uint8_t address[6];
    int8_t rssi;
} ConnectivityBluetoothDevice;

void Connectivity_Initialize(void);
void Connectivity_WifiScan(void);
size_t Connectivity_WifiCount(void);
const ConnectivityWifiAP *Connectivity_WifiGet(size_t index);
bool Connectivity_WifiScanning(void);
bool Connectivity_WifiConnected(void);
bool Connectivity_WifiConnecting(void);
const char *Connectivity_WifiSSID(void);
const char *Connectivity_WifiStatus(void);
void Connectivity_WifiConnect(const char *ssid, const char *password);
void Connectivity_BluetoothScan(void);
size_t Connectivity_BluetoothCount(void);
const ConnectivityBluetoothDevice *Connectivity_BluetoothGet(size_t index);
bool Connectivity_BluetoothScanning(void);
bool Connectivity_BluetoothConnected(void);
const char *Connectivity_BluetoothName(void);
void Connectivity_BluetoothConnect(size_t index);
size_t Connectivity_BluetoothAudioWrite(const void *data, size_t length);
