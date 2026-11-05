#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <commctrl.h>
#include <set>
#include <map>
#include <mutex>
#include <string>
#include <stdio.h>

// TeamSpeak SDK Headers
#include "plugin_definitions.h"
#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "ts3_functions.h"

#define PLUGINS_EXPORTDLL __declspec(dllexport)

#ifdef __cplusplus
extern "C" {
#endif

/* Required plugin information exports */
PLUGINS_EXPORTDLL const char* ts3plugin_name();
PLUGINS_EXPORTDLL const char* ts3plugin_version();
PLUGINS_EXPORTDLL int ts3plugin_apiVersion();
PLUGINS_EXPORTDLL const char* ts3plugin_author();
PLUGINS_EXPORTDLL const char* ts3plugin_description();
PLUGINS_EXPORTDLL void ts3plugin_setFunctionPointers(const struct TS3Functions funcs);
PLUGINS_EXPORTDLL int ts3plugin_init();
PLUGINS_EXPORTDLL void ts3plugin_shutdown();

/* Settings configuration hooks */
PLUGINS_EXPORTDLL int ts3plugin_offersConfigure();
PLUGINS_EXPORTDLL void ts3plugin_configure(void* handle, void* qParentWidget);

/* TeamSpeak event callbacks */
PLUGINS_EXPORTDLL void ts3plugin_onTalkStatusChangeEvent(
    uint64 serverConnectionHandlerID, 
    int status, 
    int isReceivedWhisper, 
    anyID clientID
);

PLUGINS_EXPORTDLL void ts3plugin_onConnectStatusChangeEvent(
    uint64 serverConnectionHandlerID, 
    int newStatus, 
    unsigned int errorNumber
);

#ifdef __cplusplus
}
#endif