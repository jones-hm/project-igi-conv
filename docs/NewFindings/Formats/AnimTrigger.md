# AnimTrigger Format

**Extension:** `.qvm` (compiled), `.qsc` (source)
**Signature:** `LOOP`
**Task Type:** N/A (animation system)

## Overview

AnimTrigger format stores animation trigger event definitions. These define when animations should play, stop, or transition based on game events.

## File Header

Same QVM 60-byte header as other LOOP-format files.

## Native Functions

| Native | Address | Description |
|--------|---------|-------------|
| AnimTriggerRead | 0x4EC070 | Read Config of AnimTrigger |
| AnimTriggerLoad | 0x4EC0C0 | Loads anim trigger objects from path `'LOCAL:animtrigger'` |

## Animation Trigger Types

| Trigger | Description |
|---------|-------------|
| Footstep | Played when foot contacts ground (animation event 1010) |
| WeaponFire | Played when weapon discharges |
| Reload | Played when reload animation starts |
| Death | Played on character death |
| HitReaction | Played when taking damage |
| LadderMount | Played when mounting/dismounting ladder |
| VehicleEnter | Played when entering vehicle |
| VehicleExit | Played when exiting vehicle |

## Animation Event IDs (from 0x4137E0 disassembly)

| Event ID | Description |
|----------|-------------|
| 1 | Fire weapon |
| 2 | Reload |
| 3 | Dry fire |
| 4 | Strike (melee) |
| 5 | Change weapon |
| 6 | Throw grenade |
| 7 | Peek |
| 8 | Map computer / binoculars |
| 9 | Fire (alternate) |
| 10 | Medipack application |

## References

- Dispatcher: `AnimationEvent_Dispatcher @ 0x4137E0` (586 instructions)
- Weapon action dispatch: `WeaponActionDispatch @ 0x4137E0`
