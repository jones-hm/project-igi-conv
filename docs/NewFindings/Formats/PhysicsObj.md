# PhysicsObj Format

**Extension:** `.qvm` (compiled), `.qsc` (source)
**Signature:** `LOOP`
**Task Type:** `TASKTYPE_PHYSICSOBJ`, `TASKTYPE_GENERICPHYSICSOBJ`

## Overview

PhysicsObj format stores physics object definitions for helicopters, cars, planes, and other vehicles. Contains mass, dimensions, torque, smoothing, and aerodynamic surface data.

## File Header

Same QVM 60-byte header as other LOOP-format files.

## Native Functions

| Native | Address | Description |
|--------|---------|-------------|
| PhysicsObjTypeRead | 0x004EDFE0 | Read Physics obj config file |
| PhysicsObjLoad | 0x004EE30 | Loads physics objects from path `'LOCAL:physicsobj'` |

## Helicopter Physics Definition

From `DefinePhysicsObjType` native and 0x431E30 disassembly:

```c
typedef struct {
    float mass;                  // Vehicle mass
    Vector3f dimensions;         // Bounding box dimensions
    Vector3f torque;             // Pitch, yaw, roll torque
    Vector3f smoothing;          // Torque smoothing factors
    float high_collective_step;  // High rotor collective angle
    float low_collective_step;   // Low rotor collective angle
    uint32_t surface_count;      // Number of aerodynamic surfaces
    AerodynamicSurface surfaces[]; // Per-surface drag data
} HelicopterPhysicsDefinition;

typedef struct {
    float coefficient;   // Drag coefficient
    Vector3f direction;  // Drag direction
    Vector3f point;      // Application point
} AerodynamicSurfaceDefinition;
```

## Rotor Physics Definition

From `RotorParser @ 0x42D9F0`:

```c
typedef struct {
    bool produces_lift;      // True if this rotor generates lift
    bool is_tail_rotor;      // True if this is the tail rotor
    uint32_t blade_samples;  // Number of blade animation samples
    float max_tilt;          // Maximum tilt angle (radians, converted from degrees)
    float phase_step;        // Angular phase per unit of collective per tick
    char* sound;             // Rotor sound effect name
} RotorPhysicsDefinition;
```

## Vehicle Struct Offsets (from disassembly)

| Offset | Type | Purpose |
|--------|------|---------|
| +0x70 | Quaternion | Rotation |
| +0xF0 | Vector3f | Position |
| +0x1B4 | PhysicsConfig* | Physics definition pointer |
| +0x720 | float | Throttle |
| +0x724-0x72C | float[3] | Smoothed torque (pitch, yaw, roll) |
| +0x730 | float | Target torque |
| +0x734-0x73C | float[3] | Smoothing factors |
| +0x748 | int | Gear visual state |
| +0x758 | float | Mass |

## TASKTYPE Constants for Physics Objects

- `TASKTYPE_CAR` — Car physics
- `TASKTYPE_HELI` — Helicopter physics
- `TASKTYPE_PLANE` — Plane physics
- `TASKTYPE_WHEEL` — Wheel physics
- `TASKTYPE_ROTOR` — Rotor physics
- `TASKTYPE_COCKPITSHIELD` — Cockpit shield
- `TASKTYPE_GEAR` — Landing gear
- `TASKTYPE_HATCH` — Vehicle hatch
- `TASKTYPE_RUDDER` — Rudder control surface
- `TASKTYPE_SMOKE` — Smoke trail effect

## References

- Full physics: `VehicleHeliPhysics_FullUpdate @ 0x431E30` (1824 instructions)
- Rotor parsing: `RotorParser @ 0x42D9F0` (37 instructions)
- Force application: `Vehicle_ForceAppend @ 0x4ECF50` (39 instructions)
