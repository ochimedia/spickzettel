#pragma once

// Shared between app_icon.rc (the resource script that embeds the .ico
// into the .exe) and the C++ that later loads it back out via
// LoadIconA(instance, MAKEINTRESOURCE(IDI_APP_ICON)) - the overlay window's
// class icon and the tray icon. Above 100 to stay clear of any ids a future
// resource might want.
#define IDI_APP_ICON 101
