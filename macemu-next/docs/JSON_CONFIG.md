# JSON Configuration System

macemu-next uses JSON for configuration files with human-readable values.

---

## Quick Start

### Generate Default Config

```bash
./macemu-next --save-config
```

This creates `~/.config/macemu-next/config.json` with defaults.

### Use Custom Config

```bash
./macemu-next --config /path/to/config.json ~/quadra.rom
```

---

## Configuration File Locations

macemu-next searches for configuration files in this priority order:

1. **CLI Override**: `--config /path/to/config.json` (highest priority)
2. **User Config**: `~/.config/macemu-next/config.json` (XDG_CONFIG_HOME)
3. **Current Directory**: `./macemu-next.json`

If no config file is found, default values are used.

---

## Configuration Format

### Example Configuration

```json
{
  "version": "1.0",
  "emulator": {
    "cpu": "68040",
    "fpu": true,
    "ramsize": 33554432,
    "modelid": 5
  },
  "boot": {
    "bootdrive": 0,
    "bootdriver": 0
  },
  "storage": {
    "rom": "/path/to/quadra650.rom",
    "disks": [
      "/path/to/harddisk.img"
    ],
    "floppies": [],
    "cdroms": [],
    "scsi": {
      "0": null,
      "1": null,
      "2": null,
      "3": null,
      "4": null,
      "5": null,
      "6": null
    },
    "extfs": null
  },
  "video": {
    "displaycolordepth": 0,
    "screen": null,
    "frameskip": 6,
    "scale_nearest": false,
    "scale_integer": false
  },
  "audio": {
    "nosound": false,
    "sound_buffer": 0
  },
  "input": {
    "keyboardtype": 5,
    "keycodes": false,
    "keycodefile": null,
    "mousewheelmode": 0,
    "mousewheellines": 0,
    "hotkey": 0,
    "swap_opt_cmd": true,
    "init_grab": false
  },
  "network": {
    "ether": null,
    "etherconfig": null,
    "udptunnel": false,
    "udpport": 6066,
    "redir": [],
    "host_domain": []
  },
  "serial": {
    "seriala": null,
    "serialb": null
  },
  "jit": {
    "enabled": false,
    "fpu": false,
    "debug": false,
    "cachesize": 0,
    "lazyflush": false,
    "inline": false,
    "blacklist": null
  },
  "ui": {
    "nogui": false,
    "noclipconversion": false,
    "title": null,
    "gammaramp": null
  },
  "system": {
    "nocdrom": false,
    "ignoresegv": true,
    "delay": 0,
    "yearofs": 0,
    "dayofs": 0,
    "mag_rate": null,
    "name_encoding": 0,
    "xpram": null
  }
}
```

---

## Configuration Options

### Emulator Section

| Option | Type | Values | Description |
|--------|------|--------|-------------|
| `cpu` | string | `"68000"`, `"68010"`, `"68020"`, `"68030"`, `"68040"` | CPU type to emulate |
| `fpu` | boolean | `true`, `false` | Enable FPU emulation (auto-enabled for 68040) |
| `ramsize` | number | 1048576 - 1073741824 | RAM size in bytes (1MB - 1GB) |
| `modelid` | number | 0-20 | Mac model ID (Gestalt Model ID minus 6) |

**Common RAM sizes:**
- 8MB: `8388608`
- 16MB: `16777216`
- 32MB: `33554432`
- 64MB: `67108864`
- 128MB: `134217728`

### Boot Section

| Option | Type | Description |
|--------|------|-------------|
| `bootdrive` | number | Boot drive number (default: 0) |
| `bootdriver` | number | Boot driver number (default: 0) |

### Storage Section

| Option | Type | Description |
|--------|------|-------------|
| `rom` | string | Path to ROM file |
| `disks` | array | Paths to disk image files |
| `floppies` | array | Paths to floppy image files |
| `cdroms` | array | Paths to CD-ROM image files |
| `scsi.0` - `scsi.6` | string | SCSI device mappings |
| `extfs` | string | Root path for ExtFS (host filesystem access) |

**Example:**
```json
"storage": {
  "rom": "/home/user/roms/quadra650.rom",
  "disks": [
    "/home/user/images/system.img",
    "/home/user/images/data.img"
  ],
  "cdroms": [
    "/home/user/iso/macos753.iso"
  ]
}
```

### Video Section

| Option | Type | Description |
|--------|------|-------------|
| `displaycolordepth` | number | Display color depth (0=auto) |
| `screen` | string | Video mode string |
| `frameskip` | number | Frames to skip (default: 6) |
| `scale_nearest` | boolean | Use nearest-neighbor scaling |
| `scale_integer` | boolean | Use integer scaling |

### Audio Section

| Option | Type | Description |
|--------|------|-------------|
| `nosound` | boolean | Disable sound output |
| `sound_buffer` | number | Sound buffer size (0=default) |

### Input Section

| Option | Type | Description |
|--------|------|-------------|
| `keyboardtype` | number | Hardware keyboard type (default: 5) |
| `keycodes` | boolean | Use keycodes vs keysyms |
| `keycodefile` | string | Path to keycode translation file |
| `mousewheelmode` | number | Mouse wheel mode (0=page up/down, 1=cursor) |
| `mousewheellines` | number | Lines to scroll in mode 1 |
| `hotkey` | number | Hotkey modifier |
| `swap_opt_cmd` | boolean | Swap Option and Command keys |
| `init_grab` | boolean | Initially grab mouse |

### Network Section

| Option | Type | Description |
|--------|------|-------------|
| `ether` | string | Ethernet device name |
| `etherconfig` | string | Network config script path |
| `udptunnel` | boolean | Tunnel packets over UDP |
| `udpport` | number | UDP port for tunneling (default: 6066) |
| `redir` | array | Port forwarding rules (for slirp) |
| `host_domain` | array | Domains to handle on host (slirp only) |

### Serial Section

| Option | Type | Description |
|--------|------|-------------|
| `seriala` | string | Serial port A device |
| `serialb` | string | Serial port B device |

### JIT Section

| Option | Type | Description |
|--------|------|-------------|
| `enabled` | boolean | Enable JIT compiler (UAE backend only) |
| `fpu` | boolean | Enable JIT FPU compilation |
| `debug` | boolean | Enable JIT debugger |
| `cachesize` | number | Translation cache size in KB |
| `lazyflush` | boolean | Lazy invalidation of cache |
| `inline` | boolean | Inline constant jumps |
| `blacklist` | string | Blacklist opcodes from translation |

### UI Section

| Option | Type | Description |
|--------|------|-------------|
| `nogui` | boolean | Disable GUI |
| `noclipconversion` | boolean | Don't convert clipboard contents |
| `title` | string | Window title |
| `gammaramp` | string | Gamma ramp setting ("on", "off", "fullscreen") |

### System Section

| Option | Type | Description |
|--------|------|-------------|
| `nocdrom` | boolean | Don't install CD-ROM driver |
| `ignoresegv` | boolean | Ignore illegal memory accesses |
| `delay` | number | Additional delay in µs every 64k instructions |
| `yearofs` | number | Year offset for date/time |
| `dayofs` | number | Day offset for date/time |
| `mag_rate` | string | Magnification rate |
| `name_encoding` | number | File name encoding |
| `xpram` | string | Path to XPRAM file |

---

## Command-Line Options

### Configuration Management

```bash
# Use specific config file
--config <file>

# Save current settings to user config
--save-config
```

### Override Individual Settings

You can still override individual settings via command-line:

```bash
./macemu-next --config myconfig.json --cpu 4 --ramsize 67108864 ~/quadra.rom
```

Command-line options override config file settings.

---

## Migration from Old Format

The old `BasiliskII_Prefs` text format is **no longer supported**.

To migrate:

1. Run macemu-next once with `--save-config`
2. Edit the generated JSON file at `~/.config/macemu-next/config.json`
3. Set your ROM path and other preferences
4. Remove old `BasiliskII_Prefs` file

---

## Tips

### Per-ROM Configurations

Create different config files for different ROMs:

```bash
# Create configs
./macemu-next --config quadra.json --save-config
./macemu-next --config macii.json --save-config

# Use them
./macemu-next --config quadra.json ~/roms/quadra650.rom
./macemu-next --config macii.json ~/roms/macii.rom
```

### Minimal Config

You only need to specify values that differ from defaults:

```json
{
  "version": "1.0",
  "emulator": {
    "cpu": "68040",
    "ramsize": 67108864
  },
  "storage": {
    "rom": "/home/user/quadra.rom",
    "disks": ["/home/user/system.img"]
  }
}
```

Missing options will use default values.

---

## Troubleshooting

### Config File Not Found

```
No config file found, using defaults
To create a config file, run with --save-config
```

**Solution**: Run `./macemu-next --save-config` to create default config.

### JSON Parse Error

```
ERROR: Failed to parse JSON config: ...
```

**Solution**: Check JSON syntax. Use a validator like `jq`:

```bash
jq . ~/.config/macemu-next/config.json
```

### Invalid CPU Type

```
WARNING: Unknown CPU type 'xyz', defaulting to 68030
```

**Solution**: Use valid CPU names: `"68000"`, `"68010"`, `"68020"`, `"68030"`, `"68040"`

---

## Examples

### Gaming Configuration (68040, lots of RAM)

```json
{
  "version": "1.0",
  "emulator": {
    "cpu": "68040",
    "fpu": true,
    "ramsize": 134217728
  },
  "storage": {
    "rom": "/home/user/quadra650.rom",
    "disks": ["/home/user/games.img"]
  },
  "audio": {
    "nosound": false
  }
}
```

### Compatibility Mode (68020, minimal RAM)

```json
{
  "version": "1.0",
  "emulator": {
    "cpu": "68020",
    "fpu": false,
    "ramsize": 8388608
  },
  "storage": {
    "rom": "/home/user/macii.rom",
    "disks": ["/home/user/system7.img"]
  }
}
```

---

**Last Updated**: January 4, 2026
