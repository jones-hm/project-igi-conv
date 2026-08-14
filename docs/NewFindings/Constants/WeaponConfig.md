# Weapon Configuration

## Overview

Weapon configuration stores all weapon parameters: fire rate, recoil, spread, magazine size, ammo types, and model associations.

## DefineWeaponType (QVM native)

Registers weapon types from `weaponconfig.qvm`.

| Parameter | Type | Description |
|-----------|------|-------------|
| weapon_id | int | Unique weapon ID |
| name | string | Weapon name |
| category | int | Weapon category (pistol, rifle, shotgun, etc.) |
| fire_mode | int | Single/burst/auto flags |
| fire_rate | float | Seconds between shots |
| magazine_size | int | Rounds per magazine |
| reserve_ammo | int | Maximum reserve ammunition |
| recoil_vertical | float | Vertical recoil in radians |
| recoil_horizontal | float | Horizontal recoil in radians |
| spread_base | float | Base cone spread in radians |
| spread_max | float | Maximum cone spread |
| damage | float | Base damage per hit |
| bullet_speed | float | Bullet velocity (units/sec) |
| bullet_range | float | Maximum bullet travel distance |
| reload_time | float | Reload animation duration |
| model_name | string | 3D model MEF file |
| fire_sound | string | Fire sound effect ID |
| reload_sound | string | Reload sound effect ID |
| dryfire_sound | string | Dry fire sound ID |

## DefineAmmoType (QVM native)

| Parameter | Type | Description |
|-----------|------|-------------|
| ammo_id | int | Ammo type ID |
| name | string | Ammo name |
| display_type | int | HUD display type (clip, shell, grenade, etc.) |
| damage | float | Damage per round |
| penetration | float | Armor penetration |

## Weapon IDs (from TASKTYPE and WEAPON_ constants)

| ID | Name | Category |
|----|------|----------|
| 0 | WEAPONTYPE_PISTOL | Sidearm |
| 1 | WEAPONTYPE_SHOTGUN | Shotgun |
| 2 | WEAPONTYPE_GUN | Rifle/SMG |
| 3 | WEAPONTYPE_KNIFE | Melee |
| 4 | WEAPONTYPE_GRENADE | Thrown |
| 5 | WEAPONTYPE_MEDIPACK | Healing |
| 6 | WEAPONTYPE_BINOCULAR | Tool |
| 7 | WEAPONTYPE_PROXIMITYMINE | Deployable |

## Ammo IDs

| ID | Name | Display Type |
|----|------|--------------|
| 0 | AMMO_ID_M203 | Grenade |
| 1 | AMMO_ID_MEDIPACK | Normal |

## Display Types

| Type | Description |
|------|-------------|
| AMMODISPLAYTYPE_NORMAL | Standard bullet counter |
| AMMODISPLAYTYPE_CLIP | Clip icon |
| AMMODISPLAYTYPE_SHELL | Shell icon |
| AMMODISPLAYTYPE_BARREL | Barrel icon |
| AMMODISPLAYTYPE_GRENADE | Grenade icon |
| AMMODISPLAYTYPE_NONE | No display |

## Native Functions

| Native | Address | Description |
|--------|---------|-------------|
| WeaponConfigRead | 0x4071E0 | Read weapon config |
| AmmoTypeOpen | 0x47CAB0 | Open ammo qvm |
| WeaponTypeOpen | 0x413B70 | Open weapons |
| GunPickup | 0x45FFC0 | Pickup weapon |
| WeaponGunPickup | 0x45FFC0 | Weapon pickup handler |
| WeaponAmmoPickup | 0x45FF80 | Ammo pickup |
| WeaponsCountGet | 0x413BB0 | Get total weapon count |

## References

- Weapon state: `WeaponState_Update @ 0x411000`
- Fire resolver: `FireResolver @ 0x462E50`
- Full simulation: `Gun_FullSimulation @ 0x47B750`
- Recoil: `Gun_RecoilApply @ 0x47C610`
- Bullet trace: `Gun_BulletTrace @ 0x47A260`
- Impact effect: `Gun_ImpactEffect @ 0x47A360`
