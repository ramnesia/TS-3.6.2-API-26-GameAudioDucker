#include "plugin.h"

#define STATUS_NOT_TALKING 0
#define STATUS_TALKING     1

// Control IDs for GUI
#define ID_SLIDER        1001
#define ID_LABEL_VOL     1002
#define ID_CHECK_SELF    1003
#define ID_CHECK_OTHERS  1004
#define ID_BTN_SAVE      1005
#define ID_BTN_CANCEL    1006
#define ID_BTN_UNINSTALL 1007

static struct TS3Functions ts3Functions;
static std::mutex g_audioMutex;
static std::set<anyID> g_activeTalkers;
static std::map<DWORD, float> g_savedVolumes;
static bool g_isDucked = false;

// Active settings
static float g_duckMultiplier = 0.25f;
static bool  g_duckOnSelf     = true;
static bool  g_duckOnOthers   = true;
static char  g_iniPath[MAX_PATH] = {0};

static HINSTANCE g_hInst = NULL;
static HWND      g_hSettingsWnd = NULL;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hInst = hinstDLL;
    }
    return TRUE;
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

    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        WritePrivateProfileStringA("Settings", "DuckPercentage", "25", g_iniPath);
        WritePrivateProfileStringA("Settings", "DuckOnSelf", "1", g_iniPath);
        WritePrivateProfileStringA("Settings", "DuckOnOthers", "1", g_iniPath);
    }

    int percent = GetPrivateProfileIntA("Settings", "DuckPercentage", 25, g_iniPath);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_duckMultiplier = (float)percent / 100.0f;

    g_duckOnSelf   = (GetPrivateProfileIntA("Settings", "DuckOnSelf", 1, g_iniPath) != 0);
    g_duckOnOthers = (GetPrivateProfileIntA("Settings", "DuckOnOthers", 1, g_iniPath) != 0);
}

static void SaveConfig(int percent, bool duckSelf, bool duckOthers) {
    if (g_iniPath[0] == '\0') InitConfigPath();

    g_duckMultiplier = (float)percent / 100.0f;
    g_duckOnSelf     = duckSelf;
    g_duckOnOthers   = duckOthers;

    char pctBuf[16];
    snprintf(pctBuf, sizeof(pctBuf), "%d", percent);

    WritePrivateProfileStringA("Settings", "DuckPercentage", pctBuf, g_iniPath);
    WritePrivateProfileStringA("Settings", "DuckOnSelf", duckSelf ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("Settings", "DuckOnOthers", duckOthers ? "1" : "0", g_iniPath);
}

// ----------------------------------------------------------------------
// Uninstallation Routine
// ----------------------------------------------------------------------
static void ExecuteUninstall(HWND hwnd) {
    int res = MessageBoxA(hwnd, 
        "Are you sure you want to uninstall Game Audio Ducker?\n\n"
        "This will delete your settings and remove the plugin the moment TeamSpeak closes.",
        "Uninstall Plugin", 
        MB_YESNO | MB_ICONWARNING);

    if (res != IDYES) return;

    if (g_iniPath[0] != '\0') {
        DeleteFileA(g_iniPath);
    }

    char dllPath[MAX_PATH] = {0};
    GetModuleFileNameA(g_hInst, dllPath, MAX_PATH);

    char cmdArgs[1024];
    snprintf(cmdArgs, sizeof(cmdArgs), 
        "/C powershell -WindowStyle Hidden -Command \""
        "Start-Sleep -Seconds 1; "
        "while (Get-Process ts3client_win64 -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 1 } "
        "Remove-Item -Path '%s' -Force -ErrorAction SilentlyContinue\"",
        dllPath);

    ShellExecuteA(NULL, "open", "cmd.exe", cmdArgs, NULL, SW_HIDE);

    MessageBoxA(hwnd, 
        "Uninstallation complete!\n\nPlease close TeamSpeak 3 to finish removing the plugin.", 
        "Uninstalled", 
        MB_OK | MB_ICONINFORMATION);

    DestroyWindow(hwnd);
}

// ----------------------------------------------------------------------
// Windows Audio Ducking (Scans ALL Sonar & Hardware Endpoints)
// ----------------------------------------------------------------------
static void SetOtherAppsDucking(bool duck) {
    CoInitialize(NULL);
    IMMDeviceEnumerator* enumerator = nullptr;

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, 
                                __uuidof(IMMDeviceEnumerator), (void**)&enumerator)) || !enumerator) {
        CoUninitialize();
        return;
    }

    // Enumerate ALL active playback endpoints (Gaming, Chat, Media, Aux, Headset)
    IMMDeviceCollection* deviceCollection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &deviceCollection)) && deviceCollection) {
        UINT deviceCount = 0;
        deviceCollection->GetCount(&deviceCount);
        DWORD myPid = GetCurrentProcessId();

        for (UINT d = 0; d < deviceCount; ++d) {
            IMMDevice* device = nullptr;
            if (SUCCEEDED(deviceCollection->Item(d, &device)) && device) {
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

                                    // Skip TeamSpeak itself and system sounds (PID 0)
                                    if (pid != 0 && pid != myPid) {
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
    static HWND hSlider, hLabelVol, hCheckSelf, hCheckOthers;

    switch (msg) {
        case WM_CREATE: {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            char volText[64];
            int currentPct = (int)(g_duckMultiplier * 100.0f);
            snprintf(volText, sizeof(volText), "Ducked Game Volume: %d%%", currentPct);

            hLabelVol = CreateWindowA("STATIC", volText, WS_CHILD | WS_VISIBLE, 
                                      20, 15, 320, 20, hwnd, (HMENU)ID_LABEL_VOL, NULL, NULL);
            SendMessage(hLabelVol, WM_SETFONT, (WPARAM)hFont, TRUE);

            hSlider = CreateWindowA(TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ, 
                                    15, 40, 335, 35, hwnd, (HMENU)ID_SLIDER, NULL, NULL);
            SendMessage(hSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessage(hSlider, TBM_SETPOS, TRUE, currentPct);
            SendMessage(hSlider, TBM_SETTICFREQ, 10, 0);

            hCheckSelf = CreateWindowA("BUTTON", "Lower game volume when I speak", 
                                       WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 
                                       20, 85, 320, 24, hwnd, (HMENU)ID_CHECK_SELF, NULL, NULL);
            SendMessage(hCheckSelf, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hCheckSelf, BM_SETCHECK, g_duckOnSelf ? BST_CHECKED : BST_UNCHECKED, 0);

            hCheckOthers = CreateWindowA("BUTTON", "Lower game volume when others speak", 
                                         WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 
                                         20, 115, 320, 24, hwnd, (HMENU)ID_CHECK_OTHERS, NULL, NULL);
            SendMessage(hCheckOthers, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hCheckOthers, BM_SETCHECK, g_duckOnOthers ? BST_CHECKED : BST_UNCHECKED, 0);

            HWND hBtnUninstall = CreateWindowA("BUTTON", "Uninstall", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                                               20, 160, 95, 28, hwnd, (HMENU)ID_BTN_UNINSTALL, NULL, NULL);
            SendMessage(hBtnUninstall, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hBtnSave = CreateWindowA("BUTTON", "Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 
                                          165, 160, 85, 28, hwnd, (HMENU)ID_BTN_SAVE, NULL, NULL);
            SendMessage(hBtnSave, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hBtnCancel = CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                                            260, 160, 85, 28, hwnd, (HMENU)ID_BTN_CANCEL, NULL, NULL);
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

                SaveConfig(pos, selfVal, othersVal);
                DestroyWindow(hwnd);
            } else if (wmId == ID_BTN_CANCEL) {
                DestroyWindow(hwnd);
            } else if (wmId == ID_BTN_UNINSTALL) {
                ExecuteUninstall(hwnd);
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
    icex.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    const char CLASS_NAME[] = "TS3GameAudioDuckerSettings";

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = SettingsWndProc;
    wc.hInstance     = g_hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    int width = 380;
    int height = 245;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    g_hSettingsWnd = CreateWindowExA(
        WS_EX_TOPMOST,
        CLASS_NAME,
        "Game Audio Ducker - Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, width, height,
        NULL,
        NULL, 
        g_hInst, 
        NULL
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
    return "1.4";
}

PLUGINS_EXPORTDLL int ts3plugin_apiVersion() {
    return 26;
}

PLUGINS_EXPORTDLL const char* ts3plugin_author() {
    return "RAM";
}

PLUGINS_EXPORTDLL const char* ts3plugin_description() {
    return "Lowers game volume when anyone or you speak.";
}

PLUGINS_EXPORTDLL void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) {
    ts3Functions = funcs;
}

int ts3plugin_init() {
    InitConfigPath();
    LoadConfig();
    return 0;
}

void ts3plugin_shutdown() {
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