# Warhammer 40,000 Gladius Supreme Companion Tools

Companion daemons, screensaver dispatchers, and audio enhancements for the [Warhammer 40,000 Gladius: Relics of War Supreme Theme](https://github.com/Semperius/omarchy-gladius-supreme-theme) on [Omarchy Linux](https://omarchy.org/).

## Features Included

- **Volumetric Smoke Trail Daemon (`gladius-smoke-trail`)**:
  - Native GTK4 layer-shell daemon drawing adaptive atmospheric smoke puffs behind the cursor.
  - Features adaptive polling (~28 Hz idle backoff), 60 FPS capped rendering, and a radial wake-burst halo.
- **Hardware-Accelerated Cinematic Screensaver (`gladius-screensaver-lock`, `gladius-screensaver-runner`)**:
  - Full-motion 1080p 60 FPS multi-monitor battle cinematics across all active monitors (`DP-1`, `DP-2`, `DP-3`).
  - Spatial audio routing: focused display plays audio at full volume, secondary displays are muted to avoid acoustic clash.
  - Seamless faction battle rotation without transitions.
- **Zero-Exposure Privacy Shield Lock (`gladius-screensaver-dismiss`)**:
  - Mutex-protected privacy shield holding visual video coverage until the Wayland `ext-session-lock-v1` lock screen is verified active.
  - Eliminates the security violation where the unlocked desktop was exposed before the login prompt appeared.
- **Adeptus Mechanicus Audio Authentication (`gladius-auth-sound`)**:
  - High-clarity cybernetic Tech-Priest voice lines praising the Omnissiah.
  - Features spoken binaric cant, FSK telemetry data packet bursts, and authentic Cult Mechanicus liturgies (Liturgy of Ignition, Awakened Cog, Rejection, Severed Logic).
- **24-Hour Artwork & Soundtrack Rotation Timer**:
  - Systemd timer rotating terminal ASCII art and official game soundtracks on a 24-hour cycle with 72-hour cooldowns.

---

## Quick Installation

```bash
git clone https://github.com/Semperius/omarchy-gladius-tools.git
cd omarchy-gladius-tools
./install.sh
```

---

## Configuration & Keybindings

Add to `~/.config/hypr/bindings.lua`:
```lua
-- Screensaver with instant secure lock
o.bind("SUPER + SHIFT + L", "Warhammer 40k Gladius Screensaver", "/home/corezon_prime/.local/bin/gladius-screensaver-lock")
```

Add floating window rule to `~/.config/hypr/hyprland.lua`:
```lua
o.window("gladius.screensaver", { fullscreen = true, float = true })
```
