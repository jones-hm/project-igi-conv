# Material Types

## Overview

Material types define surface properties for collision response, sound effects, and visual feedback.

## Material Type Enum

| ID | Name | Description |
|----|------|-------------|
| 0 | MATERIAL_CONCRETE | Concrete surface |
| 1 | MATERIAL_METAL | Metal surface |
| 2 | MATERIAL_WOOD | Wood surface |
| 3 | MATERIAL_GLASS | Glass surface (breakable) |
| 4 | MATERIAL_WATER | Water surface |
| 5 | MATERIAL_DIRT | Dirt/ground |
| 6 | MATERIAL_GRASS | Grass surface |
| 7 | MATERIAL_SAND | Sand surface |
| 8 | MATERIAL_SNOW | Snow surface |
| 9 | MATERIAL_ICE | Ice surface (low friction) |
| 10 | MATERIAL_METAL_GRATE | Metal grating |
| 11 | MATERIAL_TILE | Tile floor |
| 12 | MATERIAL_CARPET | Carpet surface |
| 13 | MATERIAL_PLASTIC | Plastic surface |
| 14 | MATERIAL_RUBBER | Rubber surface |
| 15 | MATERIAL_LEAVES | Leaf litter |

## DefineGameMaterial (QVM native)

Registers material types from `material.qvm`.

| Parameter | Type | Description |
|-----------|------|-------------|
| p1 | int | Material ID |
| name | string | Material name |
| friction | float | Friction coefficient |
| restitution | float | Bounciness |
| sound_footstep | string | Footstep sound ID |
| sound_impact | string | Impact sound ID |
| sound_bullet | string | Bullet impact sound |

## References

- Loader: `GameMaterialLoad @ 0x408350`
- Define function: `DefineGameMaterial @ 0x538520`
- QVM native registration: `DefineQMaterial @ 0x538534`
