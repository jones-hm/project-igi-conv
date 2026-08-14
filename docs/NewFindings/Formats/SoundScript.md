# Sound Script Format

**Extension:** `.qvm` (compiled), `.qsc` (source)
**Signature:** `LOOP`
**Task Type:** N/A (sound system)

## Overview

Sound script format stores level sound configuration data. Loaded via `SoundLoad` with path `'MISSION:sounds'`.

## Native Functions

| Native | Address | Description |
|--------|---------|-------------|
| SoundLoad | 0x4E68D0 | Load all level sounds from path |

## Sound Categories (GOSound constants)

From IGI.exe strings at 0x5332cc-0x5332ec:

| Constant | Address | Description |
|----------|---------|-------------|
| GOSoundSpeech | 0x5332CC | Speech/dialogue volume |
| GOSoundMusic | 0x5332DC | Music volume |
| GOSoundFX | 0x5332EC | Sound effects volume |

## Sound Effect Catalog

From IGI.exe string analysis — 200+ sound effects found:

### Weapon Sounds
- `guns_dry_1` — Dry fire click
- `grenade_tick_1` — Grenade pin pull
- Weapon fire loops (various)
- Weapon tail echoes

### Player Sounds
- `player_fall_1` through `player_fall_3` — Falling sounds (3 severity levels)
- `player_death_*` — Death sounds
- `player_hit_*` — Hit reaction sounds
- `player_xplhit_*` — Explosion hit sounds
- `jump_1` — Jump sound
- `wire_slide_1` — Wire slide sound
- `wire_drag_%d` — Wire drag variants
- `punched_01`, `punched_02` — Melee hit sounds
- `walk_ladder_%d` — Ladder climbing sounds
- `picklock_loop`, `pickloop_end` — Lock picking
- `typecomp_loop`, `typecomp_end` — Computer typing

### AI Sounds
- `GOSoundSpeech` — AI voice lines
- Combat barks
- Death sounds
- Alarm calls
- Radio chatter

### Environmental Sounds
- `rain_2` — Rain intensity
- `Ambient rain` — Ambient rain loop
- `Right ecco`, `Left ecco` — Echo effects
- `Steam`, `Smoke` — Particle sounds
- `elfence_spark1` — Electric fence spark

### Vehicle Sounds
- Helicopter rotor sounds (from RotorPhysicsDefinition)
- Engine sounds
- Weapon mount sounds

## ConditionalSound Flags

From `ConditionalSound @ 0x4E7200`:

| Flag | Purpose |
|------|---------|
| nPlayTick | Playback timing tick |
| isPlaying | Currently playing |
| isLastRun | Last run flag |
| isRun | Run state |

## Config Sound Functions

| Function | Address | Description |
|----------|---------|-------------|
| Config_SoundOptionsGetSpeechVolume | 0x537CB4 | Get speech volume |
| Config_SoundOptionsGetMusicVolume | 0x537CF8 | Get music volume |
| Config_SoundOptionsGetSoundsEffectsVolume | 0x537D40 | Get SFX volume |
| Config_SoundOptionsSetSpeechVolume | 0x537DAC | Set speech volume |
| Config_SoundOptionsSetMusicVolume | 0x537DF0 | Set music volume |
| Config_SoundOptionsSetSoundsEffectsVolume | 0x537E38 | Set SFX volume |
| Config_SoundOptionsGetReverseStereo | 0x537C6C | Get reverse stereo setting |
| Config_SoundOptionsSetReverseStereo | 0x537D88 | Set reverse stereo |
| Config_SoundOptionsGetSpeech | 0x537CB4 | Get speech enabled |
| Config_SoundOptionsGetMusic | 0x537CF8 | Get music enabled |
| Config_SoundOptionsGetSoundsEffects | 0x537D40 | Get SFX enabled |

## References

- Sound playback: `Sound_ConditionalPlay @ 0x4E7200`
- 3D audio: `Sound_Update3D`
- Footstep: `Sound_PlayFootstep`
- AI voice: `Sound_AIVoice`
