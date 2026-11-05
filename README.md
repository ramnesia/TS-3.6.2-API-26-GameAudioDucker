\# TeamSpeak 3 - Game Audio Ducker



A native audio ducking plugin for \*\*TeamSpeak 3.6.2 (Plugin API 26)\*\* on Windows 64-bit.



Automatically lowers background game, music, and browser volume whenever someone in your channel speaks or when you speak. Built with multi-endpoint support for virtual mixers like \*\*SteelSeries Sonar\*\*, \*\*Voicemeeter\*\*, and multi-channel hardware DACs.



\---



\## Features

\- \*\*Automatic Volume Attenuation\*\*: Drops background audio to your preferred level (0%–100%) during voice transmission.

\- \*\*Multi-Device / Virtual Channel Support\*\*: Scans all active audio endpoints, ensuring games routed to SteelSeries Sonar (\*Gaming\*, \*Media\*, \*Aux\*) are all ducked properly.

\- \*\*Customizable In-Client GUI\*\*: Access a native settings window directly from TeamSpeak (`Tools` → `Options` → `Addons` → `Settings`).

\- \*\*Independent Toggles\*\*: Choose whether to duck when you speak, when teammates speak, or both.

\- \*\*Safe Native Uninstallation\*\*: Includes both an in-GUI uninstaller and support for TeamSpeak's native package uninstaller.



\---



\## Installation

1\. Go to the \[Releases](../../releases) tab on the right side of this repository.

2\. Download `GameAudioDucker.ts3\_plugin`.

3\. Double-click the file and click \*\*Install\*\*.

4\. In TeamSpeak 3, navigate to \*\*Tools\*\* → \*\*Options\*\* (`Alt + P`) → \*\*Addons\*\* and ensure \*\*Game Audio Ducker\*\* is enabled.



\---



\## Configuration

Inside TeamSpeak:

1\. Open \*\*Tools\*\* → \*\*Options\*\* → \*\*Addons\*\*.

2\. Select \*\*Game Audio Ducker\*\* and click \*\*Settings\*\*.

3\. Adjust the slider to set your desired background volume percentage, toggle whether to duck for your own mic or teammates, and click \*\*Save\*\*.



\---



\## Building from Source

Built using Microsoft Visual C++ (VS 2019 Build Tools):



```cmd

cl /LD /O2 /EHsc /std:c++17 /I include plugin.cpp Ole32.lib User32.lib Comctl32.lib Gdi32.lib /Fe:GameAudioDucker_win64.dll 

