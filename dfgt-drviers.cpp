#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>

#include "sqlite3.h"
#include <iostream>
#include <vector>
#include <string>

#include <dinput.h>
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

#define LOGITECH_VID 0x046D
#define DFGT_PID     0xC29A

LPDIRECTINPUT8 dinput = nullptr;
LPDIRECTINPUTDEVICE8 dfgt_device = nullptr;

sqlite3* db = nullptr;

BOOL CALLBACK EnumDevicesCallback(const DIDEVICEINSTANCE* instance, VOID* context) {
    std::wcout << L"[DI] Found: " << instance->tszInstanceName << std::endl;

    if (wcsstr(instance->tszInstanceName, L"Logitech") || wcsstr(instance->tszInstanceName, L"Driving Force")) {
        if (FAILED(dinput->CreateDevice(instance->guidInstance, &dfgt_device, nullptr))) {
            std::cerr << "Failed to connect DirectInput device." << std::endl;
            return DIENUM_CONTINUE;
        }
        return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

bool initSQLite(const char* db_file = "dfgt_log.db") {
    if (sqlite3_open(db_file, &db) != SQLITE_OK) {
        std::cerr << "Failed to open SQLite database: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    const char* create_table_sql = R"(
        CREATE TABLE IF NOT EXISTS dfgt_input_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            steering_hid INTEGER,
            brake_hid INTEGER,
            throttle_hid INTEGER,
            steering_di INTEGER,
            pedal_di INTEGER
        );
    )";


    char* errMsg = nullptr;
    if (sqlite3_exec(db, create_table_sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "Failed to create table: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

void logToSQLiteFull(uint16_t steering, int brake, int throttle, const std::string& buttonList) {
    sqlite3* db;
    if (sqlite3_open("dfgt_full_log.db", &db) != SQLITE_OK) return;

    std::string sql = "INSERT INTO dfgt_input_full (steering, brake, throttle, buttons) VALUES (" +
        std::to_string(steering) + "," +
        std::to_string(brake) + "," +
        std::to_string(throttle) + ",'" +
        buttonList + "');";

    char* errMsg = nullptr;
    sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    sqlite3_close(db);
}



void pollDirectInputFull(DIJOYSTATE& js_out) {
    ZeroMemory(&js_out, sizeof(DIJOYSTATE));
    if (!dfgt_device) return;
    if (FAILED(dfgt_device->Poll())) return;
    dfgt_device->GetDeviceState(sizeof(DIJOYSTATE), &js_out);
}

std::string decodeButtons(const std::vector<uint8_t>& buf) {
    uint8_t b1 = buf[1];
    uint8_t b2 = buf[2];
    uint8_t b3 = buf[3];
    uint8_t b4 = buf[4];

    std::vector<std::string> pressed;

    // Byte 1
    uint8_t dpad = b1 & 0x0F;
    switch (dpad) {
        case 0x00: pressed.push_back("D-Pad Up"); break;
        case 0x01: pressed.push_back("D-Pad Up-Right"); break;
        case 0x02: pressed.push_back("D-Pad Right"); break;
        case 0x03: pressed.push_back("D-Pad Down-Right"); break;
        case 0x04: pressed.push_back("D-Pad Down"); break;
        case 0x05: pressed.push_back("D-Pad Down-Left"); break;
        case 0x06: pressed.push_back("D-Pad Left"); break;
        case 0x07: pressed.push_back("D-Pad Up-Left"); break;
        case 0x08: pressed.push_back("D-Pad Neutral"); break; // Optional
    }
    if (b1 & 0x10) pressed.push_back("Cross");
    if (b1 & 0x20) pressed.push_back("Square");
    if (b1 & 0x40) pressed.push_back("Circle");
    if (b1 & 0x80) pressed.push_back("Triangle");

    // Byte 2
    if (b2 & 0x01) pressed.push_back("R1");
    if (b2 & 0x02) pressed.push_back("L1");
    if (b2 & 0x04) pressed.push_back("R2");
    if (b2 & 0x08) pressed.push_back("L2");
    if (b2 & 0x10) pressed.push_back("Select");
    if (b2 & 0x20) pressed.push_back("Start");
    if (b2 & 0x40) pressed.push_back("R3");
    if (b2 & 0x80) pressed.push_back("L3");

    // Byte 3 (shifter and dial)
    if (b3 & 0x01) pressed.push_back("Shifter Down");
    if (b3 & 0x02) pressed.push_back("Shifter Up");
    if (b3 & 0x04) pressed.push_back("Dial Press");
    if (b3 & 0x08) pressed.push_back("Plus");
    if (b3 & 0x10) pressed.push_back("Dial Right");
    if (b3 & 0x20) pressed.push_back("Dial Left");
    if (b3 & 0x40) pressed.push_back("Minus");

    if (b3 & 0x80) {
        uint8_t horn_level = buf[6];  // direction or pressure
        if (horn_level >= 0xE0) {
            pressed.push_back("Horn Full Press");
        }
        else if (horn_level >= 0xA0) {
            pressed.push_back("Horn Half-Right");
        }
        else if (horn_level >= 0x60) {
            pressed.push_back("Horn Half-Left");
        }
        else {
            pressed.push_back("Horn (Unknown level)");
        }
    }

    // Byte 9
    if (b4 & 0x01) pressed.push_back("PS Button");

    if (!pressed.empty()) {
        std::cout << "Buttons: ";
        for (const auto& btn : pressed) {
            std::cout << "[" << btn << "] ";
        }
        std::cout << "\n";
    }

    std::string result;
    for (const auto& btn : pressed) {
        result += "[" + btn + "] ";
    }
    return result;
}


void parseReport(const std::vector<uint8_t>& buf) {
    if (buf.empty()) return;
    //if (buf.size() < 10) return;

    uint16_t steering = static_cast<uint16_t>(buf[5]) | (static_cast<uint16_t>(buf[6]) << 8);
    int pedal_raw = static_cast<int>(buf[9]);

    int brake = 0;
    int throttle = 0;

    if (pedal_raw < 128) {
        throttle = (128 - pedal_raw) * 2;
    }
    else if (pedal_raw > 128) {
        brake = (pedal_raw - 128) * 2;
    }

    if (brake > 255) brake = 255;
    if (throttle > 255) throttle = 255;

    //// Compare with DI
    //DIJOYSTATE js;
    //pollDirectInputFull(js);

    //std::cout << "HID -> Steering: " << steering
    //    << " | Brake: " << brake
    //    << " | Throttle: " << throttle
    //    << " || DI → X: " << js.lX
    //    << " | Y: " << js.lY
    //    << " | Z: " << js.lZ
    //    << " | Rx: " << js.lRx
    //    << " | Ry: " << js.lRy
    //    << " | Rz: " << js.lRz
    //    << std::endl;


    std::cout << "Index: ";
    for (size_t i = 0; i < buf.size(); ++i) {
        std::cout << std::setw(2) << std::setfill('0') << i << " ";
    }
    std::cout << "\nValue: ";
    for (size_t i = 0; i < buf.size(); ++i) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(buf[i]) << " ";
    }
    std::cout << std::dec << "\n" << std::endl;

    std::string buttonStr = decodeButtons(buf);
    std::cout << "Steering: " << steering
        << " | Brake: " << brake
        << " | Throttle: " << throttle
        << " | Buttons: " << buttonStr << std::endl;

    logToSQLiteFull(steering, brake, throttle, buttonStr);
}


HANDLE findDFGT() {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO deviceInfoSet = SetupDiGetClassDevs(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA deviceInterfaceData = { 0 };
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    for (int i = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &hidGuid, i, &deviceInterfaceData); ++i) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, NULL, 0, &size, NULL);

        std::vector<uint8_t> detailDataBuf(size);
        auto detailData = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(&detailDataBuf[0]);
        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, detailData, size, NULL, NULL)) {
            HANDLE devHandle = CreateFile(detailData->DevicePath,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

            if (devHandle != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attr = { 0 };
                attr.Size = sizeof(HIDD_ATTRIBUTES);
                HidD_GetAttributes(devHandle, &attr);

                std::cout << "[?] Found device: VID=0x" << std::hex << attr.VendorID
                    << " PID=0x" << attr.ProductID << std::endl;

                // 🆕 Print the product (friendly) name here
                WCHAR nameBuffer[256];
                if (HidD_GetProductString(devHandle, nameBuffer, sizeof(nameBuffer))) {
                    std::wcout << L"    Product: " << nameBuffer << std::endl;
                }

                // 🎯 Match updated PID
                if (attr.VendorID == LOGITECH_VID && attr.ProductID == DFGT_PID) {
                    std::cout << "[+] Found DFGT at " << detailData->DevicePath << std::endl;
                    SetupDiDestroyDeviceInfoList(deviceInfoSet);
                    return devHandle;
                }

                CloseHandle(devHandle);
            }
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return INVALID_HANDLE_VALUE;
}


int main() {
    if (!initSQLite()) return 1;

    HANDLE device = findDFGT();
    if (device == INVALID_HANDLE_VALUE) {
        std::cerr << "[-] DFGT not found." << std::endl;
        return 1;
    }

    const size_t reportLength = 36;
    std::vector<uint8_t> reportBuf(reportLength);
    OVERLAPPED overlapped = { 0 };
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    std::cout << "[*] Listening for input reports..." << std::endl;

    if (FAILED(DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION,
        IID_IDirectInput8, (void**)&dinput, nullptr))) {
        std::cerr << "DirectInput init failed." << std::endl;
        return 1;
    }
    dinput->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumDevicesCallback, nullptr, DIEDFL_ATTACHEDONLY);

    if (!dfgt_device) {
        std::cerr << "DirectInput: DFGT not found." << std::endl;
        return 1;
    }

    dfgt_device->SetDataFormat(&c_dfDIJoystick);
    dfgt_device->SetCooperativeLevel(GetConsoleWindow(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    dfgt_device->Acquire();

    while (true) {
        DWORD bytesRead;
        ReadFile(device, reportBuf.data(), reportLength, &bytesRead, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);

        parseReport(reportBuf);
    }

    CloseHandle(device);

    if (dfgt_device) {
        dfgt_device->Unacquire();
        dfgt_device->Release();
    }
    if (dinput) {
        dinput->Release();
    }

    sqlite3_close(db); // on exit
    return 0;
}
