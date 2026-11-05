#include "plugin.h"

#define STATUS_NOT_TALKING 0
#define STATUS_TALKING     1

// Control IDs for GUI
#define ID_SLIDER        1001
#define ID_LABEL_VOL     1002
#define ID_LISTVIEW_DEV  1003
#define ID_CHECK_SELF    1004
#define ID_CHECK_OTHERS  1005
#define ID_BTN_SAVE      1006
#define ID_BTN_CANCEL    1007

static struct TS3Functions ts3Functions;
static std::mutex g_audioMutex;
static std::set<anyID> g_activeTalkers;
static std::map<DWORD, float> g_savedVolumes;
static bool g_isDucked = false;

// Active settings
static float g_duckMultiplier = 0.25f;
static bool  g_duckOnSelf     = true;
static bool  g_duckOnOthers   = true;
static std::set<std::wstring> g_duckedDevices;
static char  g_iniPath[MAX_PATH] = {0};

static HINSTANCE g_hInst = NULL;
static HWND      g_hSettingsWnd = NULL;

static const PROPERTYKEY PKEY_Device_FriendlyName_Custom = { 
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 
};

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hInst = hinstDLL;
    }
    return TRUE;
}

// ----------------------------------------------------------------------
// String Conversion Helpers
// ----------------------------------------------------------------------
static std::string WStringToStr(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    if (size <= 1) return std::string();
    std::string str(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size, NULL, NULL);
    return str;
}

static std::wstring StrToWString(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
    if (size <= 1) return std::wstring();
    std::wstring wstr(size - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size);
    return wstr;
}

// ----------------------------------------------------------------------
// Universal Process Exclusions:
// Protects TeamSpeak, Discord, Windows Audio Graph, and Virtual Mixers
// ----------------------------------------------------------------------
static bool IsProcessExcluded(DWORD pid) {
    if (pid == 0 || pid == GetCurrentProcessId()) return true;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    char imgPath[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    bool exclude = false;

    if (QueryFullProcessImageNameA(hProc, 0, imgPath, &size)) {
        const char* exeName = strrchr(imgPath, '\\');
        if (exeName) exeName++; else exeName = imgPath;

        if (_stricmp(exeName, "ts3client_win64.exe") == 0 ||
            _stricmp(exeName, "ts3client_win32.exe") == 0 ||
            _stricmp(exeName, "Discord.exe") == 0 ||
            _stricmp(exeName, "audiodg.exe") == 0 ||
            _stricmp(exeName, "SteelSeriesSonar.exe") == 0 ||
            _stricmp(exeName, "SteelSeriesGG.exe") == 0 ||
            _stricmp(exeName, "SteelSeriesGGClient.exe") == 0 ||
            _stricmp(exeName, "SteelSeriesEngine.exe") == 0 ||
            _stricmp(exeName, "voicemeeter.exe") == 0 ||
            _stricmp(exeName, "voicemeeterpro.exe") == 0 ||
            _stricmp(exeName, "voicemeeter8.exe") == 0 ||
            _stricmp(exeName, "voicemeeter8x64.exe") == 0 ||
            _stricmp(exeName, "WaveLink.exe") == 0) {
            exclude = true;
        }
    }
    CloseHandle(hProc);
    return exclude;
}

// ----------------------------------------------------------------------
// Configuration Management
// ----------------------------------------------------------------------
static void InitConfigPath() {
    char appData[MAX_PATH];
    if (GetEnvironmentVariableA("APPDATA", appData, MAX_PATH) > 0) {
        snprintf(g_iniPath, sizeof(g_iniPath), "%s\\TS3Client\\plugins\\GameAudioDucker.ini", appData);
    }
}

static void LoadConfig() {
    if (g_iniPath[0] == '\0') InitConfigPath();

    int percent = GetPrivateProfileIntA("Settings", "DuckPercentage", 25, g_iniPath);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_duckMultiplier = (float)percent / 100.0f;

    g_duckOnSelf   = (GetPrivateProfileIntA("Settings", "DuckOnSelf", 1, g_iniPath) != 0);
    g_duckOnOthers = (GetPrivateProfileIntA("Settings", "DuckOnOthers", 1, g_iniPath) != 0);

    g_duckedDevices.clear();
    wchar_t devBuf[2048] = {0};
    std::wstring wIniPath = StrToWString(g_iniPath);
    GetPrivateProfileStringW(L"Settings", L"DuckedDevices", L"", devBuf, 2048, wIniPath.c_str());

    std::wstring bufStr = devBuf;
    size_t start = 0, end = 0;
    while ((end = bufStr.find(L";", start)) != std::wstring::npos) {
        std::wstring item = bufStr.substr(start, end - start);
        if (!item.empty()) g_duckedDevices.insert(item);
        start = end + 1;
    }
    if (start < bufStr.size()) {
        std::wstring item = bufStr.substr(start);
        if (!item.empty()) g_duckedDevices.insert(item);
    }
}

static void SaveConfig(int percent, bool duckSelf, bool duckOthers, const std::vector<std::wstring>& checkedDevices) {
    if (g_iniPath[0] == '\0') InitConfigPath();

    g_duckMultiplier = (float)percent / 100.0f;
    g_duckOnSelf     = duckSelf;
    g_duckOnOthers   = duckOthers;
    g_duckedDevices.clear();

    std::wstring devJoined;
    for (const auto& dev : checkedDevices) {
        g_duckedDevices.insert(dev);
        if (!devJoined.empty()) devJoined += L";";
        devJoined += dev;
    }

    char pctBuf[16];
    snprintf(pctBuf, sizeof(pctBuf), "%d", percent);

    WritePrivateProfileStringA("Settings", "DuckPercentage", pctBuf, g_iniPath);
    WritePrivateProfileStringA("Settings", "DuckOnSelf", duckSelf ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("Settings", "DuckOnOthers", duckOthers ? "1" : "0", g_iniPath);

    std::wstring wIniPath = StrToWString(g_iniPath);
    WritePrivateProfileStringW(L"Settings", L"DuckedDevices", devJoined.c_str(), wIniPath.c_str());
}

// ----------------------------------------------------------------------
// Helper: Get Friendly Name of an Audio Endpoint
// ----------------------------------------------------------------------
static std::wstring GetDeviceName(IMMDevice* device) {
    std::wstring name;
    IPropertyStore* pProps = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName_Custom, &pv))) {
            if (pv.vt == VT_LPWSTR && pv.pwszVal) {
                name = pv.pwszVal;
            }
            PropVariantClear(&pv);
        }
        pProps->Release();
    }
    return name;
}

// ----------------------------------------------------------------------
// Windows Audio Ducking (Universal across all setups)
// ----------------------------------------------------------------------
static void SetOtherAppsDucking(bool duck) {
    CoInitialize(NULL);
    IMMDeviceEnumerator* enumerator = nullptr;

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, 
                                __uuidof(IMMDeviceEnumerator), (void**)&enumerator)) || !enumerator) {
        CoUninitialize();
        return;
    }

    IMMDeviceCollection* deviceCollection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &deviceCollection)) && deviceCollection) {
        UINT deviceCount = 0;
        deviceCollection->GetCount(&deviceCount);

        for (UINT d = 0; d < deviceCount; ++d) {
            IMMDevice* device = nullptr;
            if (SUCCEEDED(deviceCollection->Item(d, &device)) && device) {
                std::wstring devName = GetDeviceName(device);

                // Determine whether this device should duck:
                bool shouldDuck = false;
                if (g_duckedDevices.empty()) {
                    // Smart Default: Duck any device that does not say "Chat" or "Communication"
                    std::wstring lower = devName;
                    for (auto& c : lower) lower = towlower(c);
                    if (lower.find(L"chat") == std::wstring::npos && 
                        lower.find(L"communication") == std::wstring::npos) {
                        shouldDuck = true;
                    }
                } else {
                    // Saved User Preference
                    if (g_duckedDevices.find(devName) != g_duckedDevices.end()) {
                        shouldDuck = true;
                    }
                }

                if (shouldDuck) {
                    IAudioSessionManager2* sessionManager = nullptr;
                    if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&sessionManager)) && sessionManager) {
                        IAudioSessionEnumerator* sessionList = nullptr;
                        if (SUCCEEDED(sessionManager->GetSessionEnumerator(&sessionList)) && sessionList) {
                            int sessionCount = 0;
                            sessionList->GetCount(&sessionCount);

                            for (int i = 0; i < sessionCount; ++i) {
                                IAudioSessionControl* session = nullptr;
                                if (SUCCEEDED(sessionList->GetSession(i, &session))) {
                                    IAudioSessionControl2* session2 = nullptr;
                                    if (SUCCEEDED(session->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&session2))) {
                                        DWORD pid = 0;
                                        session2->GetProcessId(&pid);

                                        if (pid != 0 && !IsProcessExcluded(pid)) {
                                            ISimpleAudioVolume* volume = nullptr;
                                            if (SUCCEEDED(session2->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&volume))) {
                                                if (duck) {
                                                    float currentVol = 1.0f;
                                                    volume->GetMasterVolume(&currentVol);
                                                    if (g_savedVolumes.find(pid) == g_savedVolumes.end()) {
                                                        g_savedVolumes[pid] = currentVol;
                                                    }
                                                    volume->SetMasterVolume(currentVol * g_duckMultiplier, NULL);
                                                } else {
                                                    if (g_savedVolumes.find(pid) != g_savedVolumes.end()) {
                                                        volume->SetMasterVolume(g_savedVolumes[pid], NULL);
                                                    }
                                                }
                                                volume->Release();
                                            }
                                        }
                                        session2->Release();
                                    }
                                    session->Release();
                                }
                            }
                            sessionList->Release();
                        }
                        sessionManager->Release();
                    }
                }
                device->Release();
            }
        }
        if (!duck) {
            g_savedVolumes.clear();
        }
        deviceCollection->Release();
    }
    enumerator->Release();
    CoUninitialize();
}

// ----------------------------------------------------------------------
// Native Win32 Settings GUI
// ----------------------------------------------------------------------
static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hSlider, hLabelVol, hListView, hCheckSelf, hCheckOthers;

    switch (msg) {
        case WM_CREATE: {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            // Volume Label & Slider
            char volText[64];
            int currentPct = (int)(g_duckMultiplier * 100.0f);
            snprintf(volText, sizeof(volText), "Ducked Game Volume: %d%%", currentPct);

            hLabelVol = CreateWindowA("STATIC", volText, WS_CHILD | WS_VISIBLE, 
                                      20, 15, 340, 20, hwnd, (HMENU)ID_LABEL_VOL, NULL, NULL);
            SendMessage(hLabelVol, WM_SETFONT, (WPARAM)hFont, TRUE);

            hSlider = CreateWindowA(TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ, 
                                    15, 38, 385, 35, hwnd, (HMENU)ID_SLIDER, NULL, NULL);
            SendMessage(hSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessage(hSlider, TBM_SETPOS, TRUE, currentPct);
            SendMessage(hSlider, TBM_SETTICFREQ, 10, 0);

            // Descriptive header for users with standard OR virtual setups
            HWND hLabelChannels = CreateWindowA("STATIC", "Select audio devices/channels to lower during speech:", 
                                                WS_CHILD | WS_VISIBLE, 20, 80, 375, 18, hwnd, NULL, NULL, NULL);
            SendMessage(hLabelChannels, WM_SETFONT, (WPARAM)hFont, TRUE);

            // Device Checklist
            hListView = CreateWindowExA(0, WC_LISTVIEWA, "", 
                                        WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL, 
                                        20, 102, 375, 130, hwnd, (HMENU)ID_LISTVIEW_DEV, NULL, NULL);
            SendMessage(hListView, WM_SETFONT, (WPARAM)hFont, TRUE);
            ListView_SetExtendedListViewStyle(hListView, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);

            LVCOLUMNA lvc = {0};
            lvc.mask = LVCF_WIDTH;
            lvc.cx = 350;
            ListView_InsertColumn(hListView, 0, &lvc);

            // Populate active playback devices
            CoInitialize(NULL);
            IMMDeviceEnumerator* enumerator = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, 
                                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator)) && enumerator) {
                IMMDeviceCollection* collection = nullptr;
                if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) && collection) {
                    UINT devCount = 0;
                    collection->GetCount(&devCount);

                    for (UINT i = 0; i < devCount; ++i) {
                        IMMDevice* dev = nullptr;
                        if (SUCCEEDED(collection->Item(i, &dev)) && dev) {
                            std::wstring wName = GetDeviceName(dev);
                            if (!wName.empty()) {
                                std::string sName = WStringToStr(wName);

                                LVITEMA lvi = {0};
                                lvi.mask = LVIF_TEXT;
                                lvi.iItem = i;
                                lvi.pszText = (LPSTR)sName.c_str();
                                int itemIndex = ListView_InsertItem(hListView, &lvi);

                                bool checkState = false;
                                if (!g_duckedDevices.empty()) {
                                    checkState = (g_duckedDevices.find(wName) != g_duckedDevices.end());
                                } else {
                                    std::wstring lowerName = wName;
                                    for (auto& c : lowerName) c = towlower(c);
                                    checkState = (lowerName.find(L"chat") == std::wstring::npos &&
                                                  lowerName.find(L"communication") == std::wstring::npos);
                                }
                                ListView_SetCheckState(hListView, itemIndex, checkState ? TRUE : FALSE);
                            }
                            dev->Release();
                        }
                    }
                    collection->Release();
                }
                enumerator->Release();
            }
            CoUninitialize();

            // Checkboxes
            hCheckSelf = CreateWindowA("BUTTON", "Lower volume when I speak", 
                                       WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 
                                       20, 242, 340, 22, hwnd, (HMENU)ID_CHECK_SELF, NULL, NULL);
            SendMessage(hCheckSelf, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hCheckSelf, BM_SETCHECK, g_duckOnSelf ? BST_CHECKED : BST_UNCHECKED, 0);

            hCheckOthers = CreateWindowA("BUTTON", "Lower volume when others speak", 
                                         WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 
                                         20, 268, 340, 22, hwnd, (HMENU)ID_CHECK_OTHERS, NULL, NULL);
            SendMessage(hCheckOthers, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hCheckOthers, BM_SETCHECK, g_duckOnOthers ? BST_CHECKED : BST_UNCHECKED, 0);

            // Save and Cancel Buttons
            HWND hBtnSave = CreateWindowA("BUTTON", "Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 
                                          220, 305, 85, 28, hwnd, (HMENU)ID_BTN_SAVE, NULL, NULL);
            SendMessage(hBtnSave, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hBtnCancel = CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                                            310, 305, 85, 28, hwnd, (HMENU)ID_BTN_CANCEL, NULL, NULL);
            SendMessage(hBtnCancel, WM_SETFONT, (WPARAM)hFont, TRUE);
            break;
        }

        case WM_HSCROLL: {
            if ((HWND)lParam == hSlider) {
                int pos = (int)SendMessage(hSlider, TBM_GETPOS, 0, 0);
                char buf[64];
                snprintf(buf, sizeof(buf), "Ducked Game Volume: %d%%", pos);
                SetWindowTextA(hLabelVol, buf);
            }
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == ID_BTN_SAVE) {
                int pos = (int)SendMessage(hSlider, TBM_GETPOS, 0, 0);
                bool selfVal = (SendMessage(hCheckSelf, BM_GETCHECK, 0, 0) == BST_CHECKED);
                bool othersVal = (SendMessage(hCheckOthers, BM_GETCHECK, 0, 0) == BST_CHECKED);

                std::vector<std::wstring> checkedList;
                int count = ListView_GetItemCount(hListView);
                for (int i = 0; i < count; ++i) {
                    if (ListView_GetCheckState(hListView, i)) {
                        char textBuf[512] = {0};
                        ListView_GetItemText(hListView, i, 0, textBuf, sizeof(textBuf));
                        checkedList.push_back(StrToWString(textBuf));
                    }
                }

                SaveConfig(pos, selfVal, othersVal, checkedList);
                DestroyWindow(hwnd);
            } else if (wmId == ID_BTN_CANCEL) {
                DestroyWindow(hwnd);
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;

        case WM_DESTROY:
            g_hSettingsWnd = NULL;
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void ShowSettingsGUI() {
    if (g_hSettingsWnd != NULL) {
        SetForegroundWindow(g_hSettingsWnd);
        return;
    }

    if (!g_hInst) {
        g_hInst = GetModuleHandleA("GameAudioDucker_win64.dll");
    }

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    const char CLASS_NAME[] = "TS3GameAudioDuckerSettings";

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = SettingsWndProc;
    wc.hInstance     = g_hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    int width = 430;
    int height = 385;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    g_hSettingsWnd = CreateWindowExA(
        WS_EX_TOPMOST,
        CLASS_NAME,
        "Game Audio Ducker - Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, width, height,
        NULL, NULL, g_hInst, NULL
    );

    if (g_hSettingsWnd) {
        ShowWindow(g_hSettingsWnd, SW_SHOW);
        UpdateWindow(g_hSettingsWnd);
        SetForegroundWindow(g_hSettingsWnd);

        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
}

// ----------------------------------------------------------------------
// TeamSpeak 3 Plugin API Exports
// ----------------------------------------------------------------------
extern "C" {

PLUGINS_EXPORTDLL const char* ts3plugin_name() {
    return "Game Audio Ducker";
}

PLUGINS_EXPORTDLL const char* ts3plugin_version() {
    return "1.0";
}

PLUGINS_EXPORTDLL int ts3plugin_apiVersion() {
    return 26;
}

PLUGINS_EXPORTDLL const char* ts3plugin_author() {
    return "RAM";
}

PLUGINS_EXPORTDLL const char* ts3plugin_description() {
    return "Lowers game volume when anyone speaks. Works with standard headsets and virtual mixers.";
}

PLUGINS_EXPORTDLL void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) {
    ts3Functions = funcs;
}

int ts3plugin_init() {
    InitConfigPath();
    LoadConfig();
    return 0;
}

PLUGINS_EXPORTDLL void ts3plugin_shutdown() {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (g_isDucked) {
        SetOtherAppsDucking(false);
        g_isDucked = false;
    }
    g_activeTalkers.clear();
}

PLUGINS_EXPORTDLL int ts3plugin_offersConfigure() {
    return PLUGIN_OFFERS_CONFIGURE_NEW_THREAD;
}

PLUGINS_EXPORTDLL void ts3plugin_configure(void* handle, void* qParentWidget) {
    ShowSettingsGUI();
}

PLUGINS_EXPORTDLL void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status, int isReceivedWhisper, anyID clientID) {
    std::lock_guard<std::mutex> lock(g_audioMutex);

    anyID myID = 0;
    ts3Functions.getClientID(serverConnectionHandlerID, &myID);
    bool isMe = (clientID == myID);

    if (isMe && !g_duckOnSelf) return;
    if (!isMe && !g_duckOnOthers) return;

    if (status == STATUS_TALKING) {
        g_activeTalkers.insert(clientID);
        if (!g_isDucked) {
            SetOtherAppsDucking(true);
            g_isDucked = true;
        }
    } else if (status == STATUS_NOT_TALKING) {
        g_activeTalkers.erase(clientID);
        if (g_activeTalkers.empty() && g_isDucked) {
            SetOtherAppsDucking(false);
            g_isDucked = false;
        }
    }
}

PLUGINS_EXPORTDLL void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber) {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    g_activeTalkers.clear();
    if (g_isDucked) {
        SetOtherAppsDucking(false);
        g_isDucked = false;
    }
}

} // extern "C"
