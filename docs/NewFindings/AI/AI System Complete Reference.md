# AI System — Complete Reference

## AI Types (from HUMAN_ constants)

| Constant | Address | Description |
|----------|---------|-------------|
| HUMAN_TEAM_GOOD | 0x53F360 | Friendly AI team |
| HUMAN_TEAM_BAD | 0x53F350 | Enemy AI team |
| CRITERIA_HUMAN | 0x53F3F8 | Human criteria |
| CRITERIA_HUMAN%d | 0x53F3E4 | Human criteria (indexed) |

## AI Detection Events

| Constant | Address | Description |
|----------|---------|-------------|
| HUMANAI_DETECTIONEVENT_.*_RANGE | 0x5384C4 | Detection event wildcard |
| HUMANAI_DETECTIONEVENT_GUNSHOT_SILENCED_RANGE | 0x53DB4C | Silenced gunshot detection |
| HUMANAI_DETECTIONEVENT_GUNSHOT_RANGE | 0x53DB1C | Gunshot detection |

## AI Actions (from 0x44D0E0-0x44DA00)

| Native | Address | Description |
|--------|---------|-------------|
| AIAction_Patrol | 0x44D0E0 | Assigns patrol route |
| AIAction_Combat | 0x44D160 | Engage target in combat |
| AIAction_Dead | 0x44D260 | Trigger death state |
| AIAction_FallFlat | 0x44D2C0 | Force AI to fall flat |
| AIAction_Activate | 0x44D420 | Activate interactable object |
| AIAction_WalkToNode | 0x44D4A0 | Walk to path node |
| AIAction_RunToNode | 0x44D510 | Run to path node |
| AIAction_FireAtNode | 0x44D580 | Fire at world position |
| AIAction_FireAtTask | 0x44D630 | Fire at task object |
| AIAction_PlayAnimation | 0x44D6E0 | Play skeletal animation |
| AIAction_PlaySound | 0x44D750 | Play voice/sound |
| AIAction_MoveToEvent | 0x44D7D0 | Move to detected event |
| AIAction_LookAtEvent | 0x44D870 | Look at event origin |
| AIAction_Stunned | 0x44D8F0 | Stun state (flashbang) |
| AIAction_KickGrenade | 0x44D960 | Kick grenade reaction |
| AIAction_RunPanicking | 0x44DA00 | Panic flee |
| AIAction_Idle | 0x44DA80 | Passive idle stance |
| AIAction_SetCombat | 0x44DAE0 | Set combat mode |

## AI Functions (from 0x44DAE0-0x44E790)

| Native | Address | Description |
|--------|---------|-------------|
| AIFunction_SetViewLength | 0x44DC40 | Set primary vision distance |
| AIFunction_SetAlarmViewLength | 0x44DCD0 | Set alarm vision distance |
| AIFunction_SetViewAlpha | 0x44DD30 | Set primary cone half-angle |
| AIFunction_SetViewGamma | 0x44DD90 | Set primary detection rate |
| AIFunction_SetSecondaryViewLength | 0x44DDF0 | Set peripheral vision distance |
| AIFunction_SetSecondaryAlarmViewLength | 0x44DE80 | Set peripheral alarm distance |
| AIFunction_SetSecondaryViewAlpha | 0x44DEE0 | Set peripheral cone angle |
| AIFunction_SetSecondaryViewGamma | 0x44DF40 | Set peripheral detection rate |
| AIFunction_SetEventPriority | 0x44E0C0 | Set event evaluation priority |
| AIFunction_SetInvulnerability | 0x44E150 | Set invulnerability |
| AIFunction_SetInstantDeath | 0x44E1B0 | Force instant death on hit |
| AIFunction_SetDeathAnimation | 0x44E210 | Assign death animation |
| AIFunction_SetAlarmTriggerID | 0x44E270 | Link to alarm switch |
| AIFunction_SetAlarmControlID | 0x44E2D0 | Link to alarm panel |
| AIFunction_SetAlarmAccess | 0x44E330 | Set alarm permissions |
| AIFunction_SetGunnerID | 0x44E390 | Link to stationary gun |
| AIFunction_SetScriptIntegerValue | 0x44E6D0 | Set integer script var |
| AIFunction_SetScriptRealValue | 0x44E790 | Set float script var |
| AIFunction_GetAlarmTriggerID | 0x44E3F0 | Get alarm switch ID |
| AIFunction_GetAlarmControlID | 0x44E430 | Get alarm panel ID |
| AIFunction_GetAlarmAccess | 0x44E470 | Get alarm permissions |
| AIFunction_GetGunnerID | 0x44E4B0 | Get gunner ID |
| AIFunction_GetAlarmControlStatus | 0x44E4F0 | Query alarm status |
| AIFunction_GetGunnerStatus | 0x44E5E0 | Query gunner status |
| AIFunction_GetScriptIntegerValue | 0x44E740 | Get integer script var |
| AIFunction_GetScriptRealValue | 0x44E800 | Get float script var |
| AIFunction_GetCurrentEventType | 0x44DFA0 | Get active event type |
| AIFunction_IsEventBehind | 0x44E860 | Check if event behind AI |
| AIFunction_GetRandomValue | 0x44E000 | Generate random float |
| AIFunction_GetEventDistance | 0x44E930 | Get distance to event |
| AIFunction_GetAlarmTriggerDistance | 0x44E9B0 | Get distance to alarm |
| AIFunction_SetAnimationInterval | 0x44EAF0 | Set idle animation interval |
| AIFunction_AddAnimationEntry | 0x44EBB0 | Add weighted animation |
| AIFunction_GetAnimationToPlay | 0x44ECB0 | Select animation from pool |
| AIFunction_SendResponse | 0x44EE40 | Send response signal |
| AIFunction_RemoveAlarmActions | 0x44DBC0 | Clear alarm actions |
| AIFunction_DefaultHandler | 0x44E060 | Default fallback handler |

## AI Detection System

### View Cone Test (0x4502F0)
- Two-cone system: primary (acute) and secondary (peripheral)
- Primary cone: defined by length, half-angle (alpha), and detection rate (gamma)
- Secondary cone: wider angle, shorter range
- Alarm state increases all ranges by alarm-specific multipliers

### Detection Cascade
1. **Vision cone test** — Is target within field of view?
2. **Distance check** — Is target within detection range?
3. **Line of sight** — Is path unobstructed? (0.79 tolerance)
4. **Detection accumulation** — Detection level grows over time when visible
5. **Hearing check** — Sound events can trigger detection independently

## AI State Machine

### States
- **Idle** — Passive, no target detected
- **Alert** — Sound event heard, investigating
- **Combat** — Target detected, engaging
- **Search** — Target lost, searching last known position
- **Patrol** — Following patrol path
- **Panic** — Fleeing (unarmed or overwhelmed)
- **Dead** — Death animation playing

### Transitions
- Idle → Alert: Sound event within hearing range
- Alert → Combat: Target visually confirmed
- Combat → Search: Target lost for >N ticks
- Search → Combat: Target reacquired
- Search → Idle: Search timeout
- Any → Panic: Unarmed and threat detected
- Any → Dead: Health reaches 0

## References

- AI update: `Soldier_AIUpdate @ 0x45D5B0`
- Combat update: `Soldier_CombatUpdate @ 0x45E060`
- Vision cone: `AI_ViewConeTest @ 0x4502F0`
- Detection decay: `AI_DetectionDecay`
- Detection growth: `AI_DetectionGrow`
- Line of sight: `AI_LineOfSight @ 0x489B20`
