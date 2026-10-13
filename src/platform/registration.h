#pragma once

// Registers per user as a candidate image viewer and adds a Start menu shortcut. Windows only
// lets the user pick the default, so this then opens the Default apps page for this app.
int Register();

int Unregister();
