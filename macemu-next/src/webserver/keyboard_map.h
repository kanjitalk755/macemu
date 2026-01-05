/*
 * Browser Keycode to Mac ADB Keycode Conversion
 *
 * Converts JavaScript KeyboardEvent.keyCode values to Mac ADB scancodes.
 * This was moved from emulator to server per the IPC architecture.
 */

#ifndef KEYBOARD_MAP_H
#define KEYBOARD_MAP_H

namespace keyboard_map {

/**
 * Convert browser keycode to Mac ADB keycode
 *
 * @param browser_keycode JavaScript KeyboardEvent.keyCode value
 * @return Mac ADB scancode, or -1 if not mapped
 */
int browser_to_mac_keycode(int browser_keycode);

} // namespace keyboard_map

#endif // KEYBOARD_MAP_H
