/*
 * Browser Keycode to Mac ADB Keycode Conversion
 */

#include "keyboard_map.h"

namespace keyboard_map {

int browser_to_mac_keycode(int keycode) {
    // Letters A-Z (keycodes 65-90)
    if (keycode >= 65 && keycode <= 90) {
        static const int letter_map[] = {
            0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04,  // A-H
            0x22, 0x26, 0x28, 0x25, 0x2E, 0x2D, 0x1F, 0x23,  // I-P
            0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07,  // Q-X
            0x10, 0x06                                        // Y-Z
        };
        return letter_map[keycode - 65];
    }

    // Numbers 0-9 (keycodes 48-57)
    if (keycode >= 48 && keycode <= 57) {
        static const int number_map[] = {
            0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19
        };
        return number_map[keycode - 48];
    }

    // Special keys and symbols
    switch (keycode) {
        case 8:   return 0x33;  // Backspace
        case 9:   return 0x30;  // Tab
        case 13:  return 0x24;  // Enter
        case 16:  return 0x38;  // Shift
        case 17:  return 0x36;  // Ctrl -> Command
        case 18:  return 0x3A;  // Alt -> Option
        case 27:  return 0x35;  // Escape
        case 32:  return 0x31;  // Space
        case 37:  return 0x3B;  // Left Arrow
        case 38:  return 0x3E;  // Up Arrow
        case 39:  return 0x3C;  // Right Arrow
        case 40:  return 0x3D;  // Down Arrow
        case 46:  return 0x75;  // Delete
        case 91:  return 0x37;  // Meta (Windows/Command) -> Command
        case 186: return 0x29;  // Semicolon (;)
        case 187: return 0x18;  // Equals (=)
        case 188: return 0x2B;  // Comma (,)
        case 189: return 0x1B;  // Minus (-)
        case 190: return 0x2F;  // Period (.)
        case 191: return 0x2C;  // Slash (/)
        case 192: return 0x32;  // Backtick (`)
        case 219: return 0x21;  // Left Bracket ([)
        case 220: return 0x2A;  // Backslash (\)
        case 221: return 0x1E;  // Right Bracket (])
        case 222: return 0x27;  // Quote (')
        default:  return -1;    // Unknown/unmapped key
    }
}

} // namespace keyboard_map
