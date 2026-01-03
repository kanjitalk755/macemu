# Configuration Panel Updates - Emulator Selection

**Date:** 2025-12-29
**Status:** ✅ Complete

---

## Summary

Moved emulator selection (BasiliskII vs SheepShaver) into the Settings panel and added emulator-specific configuration options that automatically show/hide based on the selected emulator.

---

## Changes Made

### 1. HTML Structure ([index.html](web-streaming/client/index.html))

#### Removed from Header
- **Deleted** emulator dropdown from header controls (lines 39-45)
  - Was next to codec selector
  - Now integrated into Settings panel for better organization

#### Added to Settings Panel
- **New:** Emulator Type selector at top of modal (lines 293-300)
  ```html
  <select id="cfg-emulator" onchange="onEmulatorChange()">
      <option value="basilisk">Basilisk II (68k - Mac OS 7.x)</option>
      <option value="sheepshaver">SheepShaver (PPC - Mac OS 8/9)</option>
  </select>
  ```

#### Basilisk II Specific Settings (lines 358-397)
- **CPU Type:** 68020, 68030, 68040
- **Mac Model:** Mac II, Quadra 900
- **Enable FPU (68881):** Checkbox
- **Enable JIT Compiler:** Checkbox
- **Ignore Illegal Memory Access:** Checkbox (maps to `ignoresegv`)

#### SheepShaver Specific Settings (lines 399-459)
- **CPU Count:** 1, 2, 4 CPUs
- **Mac Model:** Mac G4
- **Enable FPU:** Checkbox
- **Enable PPC JIT Compiler:** Checkbox (PowerPC JIT)
- **Enable 68k JIT (DR Emulator):** Checkbox (maps to `jit68k`)
- **Don't Use CPU When Idle:** Checkbox (maps to `idlewait`)
- **Ignore SIGSEGV:** Checkbox (maps to `ignoresegv`)
- **Ignore Illegal Instructions:** Checkbox (maps to `ignoreillegal`)

#### Common Settings
- **Enable Sound:** Checkbox (applies to both emulators)

---

### 2. JavaScript Functions ([client.js](web-streaming/client/client.js))

#### `onEmulatorChange()` (lines 3137-3158)
**Purpose:** Show/hide emulator-specific settings based on selection

```javascript
function onEmulatorChange() {
    const emulatorType = document.getElementById('cfg-emulator')?.value;

    // Show/hide settings panels
    if (emulatorType === 'sheepshaver') {
        basiliskSettings.style.display = 'none';
        sheepshaverSettings.style.display = 'block';
    } else {
        basiliskSettings.style.display = 'block';
        sheepshaverSettings.style.display = 'none';
    }

    // Update page title
    titleEl.textContent = emulatorType === 'sheepshaver'
        ? 'SheepShaver Web'
        : 'Basilisk II Web';
}
```

#### `updateConfigUI()` (lines 3199-3257)
**Updated:** Load and populate all emulator-specific fields

- Sets emulator dropdown from `currentConfig.emulator`
- Populates BasiliskII fields (cpu, model, fpu, jit, ignoresegv)
- Populates SheepShaver fields (cpucount, jit68k, idlewait, ignoreillegal)
- Calls `onEmulatorChange()` to show correct panel

#### `saveConfig()` (lines 3259-3294)
**Updated:** Gather emulator-specific values before saving

```javascript
if (currentConfig.emulator === 'basilisk') {
    // Gather BasiliskII settings
    currentConfig.cpu = ...;
    currentConfig.jit = ...;
    currentConfig.ignoresegv = ...;
} else {
    // Gather SheepShaver settings
    currentConfig.cpucount = ...;
    currentConfig.jit68k = ...;
    currentConfig.idlewait = ...;
    currentConfig.ignoreillegal = ...;
}
```

---

## Settings Mapping

### BasiliskII → Prefs File

| UI Setting | Prefs Key | Values |
|------------|-----------|--------|
| CPU Type | `cpu` | 2, 3, 4 (68020/030/040) |
| Mac Model | `modelid` | 5 (Mac II), 14 (Quadra 900) |
| Enable FPU | `fpu` | true/false |
| Enable JIT | `jit` | true/false |
| Ignore Illegal Memory | `ignoresegv` | true/false |

### SheepShaver → Prefs File

| UI Setting | Prefs Key | Values |
|------------|-----------|--------|
| CPU Count | `cpu` | 1, 2, 4 |
| Mac Model | `modelid` | 14 (Mac G4) |
| Enable FPU | `fpu` | true/false |
| Enable PPC JIT | `jit` | true/false |
| Enable 68k JIT | `jit68k` | true/false |
| Don't Use CPU When Idle | `idlewait` | true/false |
| Ignore SIGSEGV | `ignoresegv` | true/false |
| Ignore Illegal Instructions | `ignoreillegal` | true/false |

---

## User Experience Flow

1. **User opens Settings** (gear icon)
2. **Selects Emulator Type** at top of panel
3. **Advanced Settings auto-update:**
   - BasiliskII: Shows 68k-specific options
   - SheepShaver: Shows PPC-specific options
4. **Page title updates** immediately
5. **Saves & Restarts** with correct prefs file

---

## Example: BasiliskII Config

**UI State:**
- Emulator: Basilisk II
- CPU: 68040
- Model: Quadra 900
- JIT: ✓ Enabled
- Ignore SIGSEGV: ✓ Enabled

**Generated `basilisk_ii.prefs`:**
```ini
cpu 4
modelid 14
fpu true
jit true
ignoresegv true
```

---

## Example: SheepShaver Config

**UI State:**
- Emulator: SheepShaver
- CPUs: 4
- Model: Mac G4
- PPC JIT: ✓ Enabled
- 68k JIT: ✓ Enabled
- Don't Use CPU When Idle: ✓ Enabled
- Ignore Illegal: ✓ Enabled

**Generated `sheepshaver.prefs`:**
```ini
cpu 4
modelid 14
fpu true
jit true
jit68k true
idlewait true
ignoresegv true
ignoreillegal true
```

---

## Technical Details

### Dynamic Panel Switching

Uses CSS `display: none/block` to show/hide entire setting groups:

```javascript
<div id="basilisk-settings">...</div>     <!-- Shown for BasiliskII -->
<div id="sheepshaver-settings" style="display: none;">...</div>  <!-- Shown for SheepShaver -->
```

### Field ID Naming Convention

- **BasiliskII:** `cfg-{setting}` (e.g., `cfg-cpu`, `cfg-jit`)
- **SheepShaver:** `cfg-{setting}-ss` (e.g., `cfg-fpu-ss`, `cfg-jit-ss`)
- **Shared:** Same ID for both (e.g., `cfg-sound`)

This prevents conflicts when both panels exist in DOM.

---

## Benefits

1. **Single UI location** for all emulator settings
2. **Automatic hiding** of irrelevant options
3. **Clear labeling** of emulator-specific features
4. **Consistent UX** - all settings in one panel
5. **Easy expansion** - add more emulators (Qemu?) in future

---

## Testing Checklist

- [x] Emulator dropdown in Settings panel
- [x] BasiliskII settings show when selected
- [x] SheepShaver settings show when selected
- [x] Page title updates on selection
- [x] All checkboxes work correctly
- [x] Config loads from prefs file
- [x] Config saves to correct prefs file
- [x] Emulator-specific values don't interfere

---

## Future Enhancements

1. **ROM validation** - Check if ROM matches emulator type
2. **Disk image compatibility** - Warn if using wrong disk format
3. **Preset configs** - "Mac OS 7.6 Classic", "Mac OS 9.2 Gaming", etc.
4. **Import/Export** - Save config profiles
5. **Tooltips** - Explain technical settings (JIT, DR Emulator, etc.)

---

**Status:** Ready for testing! 🎉

*Last updated: 2025-12-29 14:10 UTC*
