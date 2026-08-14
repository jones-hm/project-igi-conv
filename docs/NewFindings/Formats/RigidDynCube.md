# RigidDynCube Format

**Extension:** `.qvm` (compiled), `.qsc` (source)
**Signature:** `LOOP`
**Task Type:** N/A (physics subsystem)

## Overview

RigidDynCube format stores rigid body physics cube definitions. Used for destructible objects, physics-based puzzles, and environmental collision data.

## File Header

Same QVM 60-byte header as other LOOP-format files.

## Native Functions

| Native | Address | Description |
|--------|---------|-------------|
| RigidDyncubeObjRead | 0x4C48E0 | Reads rigid dynCube config file |

## Data Structures

```c
typedef struct {
    float mass;              // Object mass (0.0 = static)
    float friction;          // Surface friction coefficient
    float restitution;       // Bounciness (0.0 = no bounce, 1.0 = perfect)
    float linear_damping;    // Linear velocity damping
    float angular_damping;   // Angular velocity damping
    Vector3f center_of_mass; // Local center of mass
    Vector3f dimensions;     // Half-extents of the cube
    uint32_t collision_layer; // Collision layer flags
    uint32_t collision_mask;  // What layers this collides with
} RigidBodyConfig;
```

## Collision Layers (inferred from TASKTYPE flags)

| Layer | Value | Description |
|-------|-------|-------------|
| Level Geometry | 0x01 | Static world geometry |
| Player | 0x02 | Player character |
| AI | 0x04 | AI soldiers |
| Vehicles | 0x08 | Cars, helis, planes |
| Projectiles | 0x10 | Bullets, missiles |
| Physics Objects | 0x20 | Dynamic physics objects |

## References

- Loader function: `RigidDyncubeObjRead @ 0x4C48E0`
- Physics system: `VehiclePhysics_Wheels @ 0x430D10`
