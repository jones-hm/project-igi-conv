# MagicObject Format

**Extension:** `.qvm` (compiled), `.qsc` (source)
**Signature:** `LOOP` (same as TEX)
**Task Type:** `TASKTYPE_MAGICOBJ`

## Overview

MagicObject format stores interactive object definitions for the IGI engine. These are QVM scripts that define how objects respond to player interaction (buttons, switches, leakers, etc.).

## File Header (60 bytes)

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0x00 | 4 | char[4] | signature | `"LOOP"` (0x4C4F4F50) |
| 0x04 | 4 | uint32 | ver_major | Must be 8 |
| 0x08 | 4 | uint32 | ver_minor | Must be 5 |
| 0x0C | 4 | uint32 | of_itable | Offset to identifier table |
| 0x10 | 4 | uint32 | of_ivalue | Offset to identifier strings |
| 0x14 | 4 | uint32 | sz_itable | Size of identifier table |
| 0x18 | 4 | uint32 | sz_ivalue | Size of identifier string pool |
| 0x1C | 4 | uint32 | of_stable | Offset to string table |
| 0x20 | 4 | uint32 | of_svalue | Offset to string value pool |
| 0x24 | 4 | uint32 | sz_stable | Size of string table |
| 0x28 | 4 | uint32 | sz_svalue | Size of string value pool |
| 0x2C | 4 | uint32 | of_ctable | Offset to code (bytecode) section |
| 0x30 | 4 | uint32 | sz_ctable | Size of code section |

## Native Functions

| Native | Address | Description |
|--------|---------|-------------|
| MagicObjLoad | 0x4C4930 | Loads magic objects from path `'LOCAL:magicobj'` |

## Magic Object Types

From TASKTYPE constants:
- `TASKTYPE_MAGICOBJ` — Generic magic object
- `TASKTYPE_EXPLODEMAGICOBJ` — Explosive magic object (barrels, etc.)
- `TASKTYPE_PHYSICSMAGICOBJ` — Physics-enabled magic object
- `TASKTYPE_GENERICPHYSICSMAGICOBJ` — Generic physics magic object
- `TASKTYPE_GENERICPHYSICSOBJ` — Generic physics object
- `TASKTYPE_BONEMAGICOBJ` — Bone-attached magic object
- `TASKTYPE_EDITORMAGICOBJ` — Editor-only magic object

## QVM Opcodes (49 total)

All magic objects are compiled to QVM bytecode. See `game_qvm_v5.md` for full opcode reference.

## Example QSC Script

```c
// magicobj.qsc example
Task_New(-1, "MagicObj", "explosive_barrel", 1);
Task_DeclareParameters("health", "float");
Task_Destructible_SetHealth(100.0);
```

## References

- Loader function: `MagicObjLoad @ 0x4C4930`
- Task type registration: `TasktypeSet @ 0x4B8810`
- Binary strings found: `TASKTYPE_MAGICOBJ`, `TASKTYPE_EXPLODEMAGICOBJ`, etc.
