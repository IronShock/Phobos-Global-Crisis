# GC New Features

This document lists all features added by the **GC** build on top of **Phobos
build 48** (official tag `build-48`, commit `73aa2e30`). It is a standalone
overview; the same content is integrated into the `docs/` tree (English) and the
Chinese translations under `docs/locale/zh_CN/`.

Entries marked **(already documented)** already had an English/Chinese section in
the build-48 `docs/` tree and are listed here only for completeness.

---

## 1. Movement & Locomotion

### AdvancedDrive locomotor
A new drive-style locomotor selectable with the alias `AdvancedDrive` in a
vehicle's `Locomotor=` entry, extending the vanilla `Drive` locomotor with
reverse driving, straight-line steering and an optional hover mode. It handles
slopes, bridges, tunnels and trains/carts, and fixes off-map movement (a unit
ordered off the map drives off the edge instead of freezing). Assigning the alias
with no further keys keeps behaviour close to vanilla drive. CLSID
`{4A582751-9839-11d1-B709-00A024DDAFD1}`.

```ini
[SOMEVEHICLE]
Locomotor=AdvancedDrive
```

### AdvancedDrive reverse driving
Vehicles automatically drive backwards instead of turning around toward a target
in `FaceTargetRange`, after being hurt for `RetreatDuration` frames, when close
to the destination (`MinimumDistance`), or while area-guarding. A wide threshold
with hysteresis avoids flipping for side targets; reverse speed is scaled by
`Speed`.

```ini
[SOMEVEHICLE]
AdvancedDrive.Reverse=true
AdvancedDrive.Reverse.Speed=0.85
AdvancedDrive.Reverse.FaceTarget=true
AdvancedDrive.Reverse.FaceTargetRange=4096   ; leptons
AdvancedDrive.Reverse.MinimumDistance=2560   ; leptons
AdvancedDrive.Reverse.RetreatDuration=150    ; frames
```

### AdvancedDrive turret response while reversing **(already documented)**
`AdvancedDrive.TurretResponse=false` disables turret auto-alignment while
reversing.

```ini
[SOMEVEHICLE]
AdvancedDrive.TurretResponse=false
```

### AdvancedDrive straight-line movement
When the straight line to the destination is fully passable, the unit steers
directly at the destination instead of following the grid path. If blocked, it
falls back to pathfinding with a 60-frame cooldown.

```ini
[SOMEVEHICLE]
AdvancedDrive.StraightLine=false
```

### AdvancedDrive hover
Hovering with sinusoidal bobbing and gravity/damping settling, following terrain
height. On power loss/EMP a hovering unit can spin out of control and sink over
water.

```ini
[SOMEVEHICLE]
AdvancedDrive.Hover=false
AdvancedDrive.Hover.Tilt=true
AdvancedDrive.Hover.Height=          ; default [General] -> HoverHeight
AdvancedDrive.Hover.Dampen=          ; default [General] -> HoverDampen
AdvancedDrive.Hover.Bob=             ; default [General] -> HoverBob
AdvancedDrive.Hover.Sink=true
AdvancedDrive.Hover.Spin=true
```

### Attachment locomotor
A dedicated locomotor that makes attachment children follow the parent instead of
pathfinding. The child's layer is derived from its own height against
`AttachmentTopLayerMinHeight` / `AttachmentUndergroundLayerMaxHeight` unless it
inherits the parent's height status.

```ini
[General]
AttachmentTopLayerMinHeight=500
AttachmentUndergroundLayerMaxHeight=-256
```

### SmoothMove
Post-processes A* paths with greedy Bresenham string-pulling so units move as
directly as possible instead of zig-zagging. Global with per-TechnoType override.

```ini
[General]
SmoothMove=false

[SOMETECHNO]
SmoothMove=      ; default to [General] value
```

### Pathfinding failure throttle (fix)
After 3 consecutive path failures a unit enters a 90-frame cooldown during which
A* is not invoked. Prevents per-frame full-map A* saturation. No INI keys.

### Drag formation command
Middle-mouse drag while friendly foot units are selected previews and executes a
formation move (type scales with drag distance: Linear -> Pyramid -> Wedge). The
drag direction sets facing; impassable positions are nudged to the nearest
passable spot.

```ini
[Phobos]                     ; RA2MD.INI
FormationEnabled=true
FormationNearThreshold=1536  ; leptons
FormationFarThreshold=3072   ; leptons
FormationSpacing=512         ; leptons
FormationSpacingInfants=-1   ; leptons, default FormationSpacing / 2 (min 64)
```

---

## 2. Attachment System

### Mount points
A TechnoType declares mount points `Attachment0`, `Attachment1`, .... Each
references an `AttachmentType` and a child TechnoType; the child is created and
mounted at the configured FLH offset (optionally on the turret) and switched to
the `Attachment` locomotor. Only `UnitType` children can be attached. Mounts with
a unique `ID` are preserved across type conversion; duplicate IDs are fatal.

```ini
[AttachmentTypes]
0=MYATTACH

[SOMETECHNO]
Attachment0.Type=MYATTACH
Attachment0.TechnoType=CHILD
Attachment0.FLH=0,0,0
Attachment0.IsOnTurret=no
Attachment0.RotationAdjust=0
Attachment0.ID=MyMount
```

### AttachmentType definitions
Reusable child behaviour. All keys default, so a type may be declared empty.

```ini
[MYATTACH]
RespawnAtCreation=yes
RespawnDelay=-1                  ; 0=instant, >0=frames, <0=never
InheritCommands=yes
InheritCommands.StopCommand=yes
InheritCommands.DeployCommand=yes
InheritOwner=yes
InheritStateEffects=yes
InheritHeightStatus=yes
InheritDestruction=yes
DestructionWeapon.Child=
DestructionWeapon.Parent=
ParentDestructionMission=
ParentDetachmentMission=
OccupiesCell=yes
PassSelection=yes
LowSelectionPriority=yes
TransparentToMouse=no
VirtualUnit=no
YSortPosition=default            ; Default | UnderParent | OverParent
```

### Virtual attachment units
Makes children purely visual: unselectable, unattackable, never auto-targeted,
never pathfinding, transparent to mouse. Global or per AttachmentType.

```ini
[General]
TA.VirtualUnit=false

[MYATTACH]
VirtualUnit=false
```

---

## 3. User Interface & Hotkeys

### Tactical time stop
Singleplayer hotkey that freezes the battlefield simulation while keeping full UI
control; queued orders execute on resume. Optional per-difficulty action block.

```ini
[General]
TacticalPause=true
TacticalPause.BlockActions.Difficulty=none   ; none | easy | normal | hard
```

### Target filter buttons
Six bottom-left buttons set a bitmask target filter for the selected techno(s):
Infantry, Vehicle, Artillery, Building, Fighter, Bomber.

```ini
[ArtilleryTypes]
0=SOMETYPE,SOMEOTHERTYPE
[BomberTypes]
0=SOMETYPE
```

### Center message column
Scrollable message column with toggle/scroll buttons, records and wheel scrolling.

```ini
[Phobos]                     ; RA2MD.INI
MessageDisplayInCenter=false
MessageApplyHoverState=false
MessageDisplayInCenter.LabelsCount=6
MessageDisplayInCenter.RecordsCount=12
```

### Toggle message label
Hotkey that packs/expands the center message column.

### Reverse move / toggle auto-reverse
`Reverse Move` is a hold-to-reverse hotkey; `Toggle Auto-Reverse` flips the
default auto-reverse state for the player.

```ini
[General]
ReverseMove=false
```

### Right-click commands
Mainstream-RTS mouse scheme (left selects, right commands).

```ini
[Phobos]                     ; RA2MD.INI
RightClickCommands=false
RightClickCommands.Debug=false
```

### Cinema mode
Hotkey that hides the message list, superweapon timers and custom sidebar widgets.

### FPS counter
Hotkey toggles an FPS readout above the target-filter buttons. `ShowFPS` is
force-disabled in code.

### Game timer
Hotkey toggles the "Operation Time Elapsed" display.

```ini
[Phobos]                     ; RA2MD.INI
ShowGameTime=true
ShowGameTime.BoardOpacity=40
```

### Continue game on exit
Writes `Saved Games\ContinueGame_<map>.cgs` when quitting a campaign mission, for
the CnCNet "Continue Game" button.

```ini
[Phobos]                     ; RA2MD.INI
ContinueGameOnExit=true
```

### Tactical zoom **(already documented)**
`Ctrl + wheel` / hotkey battlefield magnification.

```ini
[TacticalZoom]               ; uimd.ini
TacticalZoom=false
TacticalZoom.Scroll=true
TacticalZoom.KeyEnabled=true
TacticalZoom.Max=3.6
TacticalZoom.Step=0.2
TacticalZoom.Smooth=true
```

### Text scaling **(already documented)**
Scales in-game bitmap text at high resolutions.

```ini
[Phobos]                     ; RA2MD.INI
TextScale=1.0
TextScale.Smooth=true
```

### Tactical Gear **(already documented)**
Two hotkeys shift game speed one step up/down.

```ini
[General]
TacticalGear=false
```

### Remove in-game Save/Load buttons by difficulty **(already documented)**
```ini
[General]
DisableSaveGame.Difficulty=none
DisableLoadGame.Difficulty=none
```

### Lock game speed by difficulty **(already documented)**
```ini
[General]
LockGameSpeed.Difficulty=none
```

---

## 4. Gameplay Logic

### Directional armor multipliers
Front/side/rear damage scaling based on the attack angle.

```ini
[CombatDamage]
Armor.FrontMultiplier=1.0
Armor.SideMultiplier=1.0
Armor.RearMultiplier=1.0
```

### AI directional armor
AI vehicles turn to present frontal armor.

```ini
[General]
AIDirectionalArmor=true
AIDirectionalArmor.Chance=1.0
AIDirectionalArmor.Delay=0
```

### Secondary ammo pool
Independent ammo pool used by the secondary weapon.

```ini
[SOMETECHNO]
Ammo.Secondary=0
Ammo.Secondary.InitialAmmo=-1
Ammo.Secondary.Reload=-1
Ammo.Secondary.EmptyReload=-1
Ammo.Secondary.ReloadIncrement=1
Ammo.Secondary.AddOnDeploy=0
Ammo.Secondary.AutoDeployMinimumAmount=-1
Ammo.Secondary.AutoDeployMaximumAmount=-1
Ammo.Secondary.DeployUnlockMinimumAmount=-1
Ammo.Secondary.DeployUnlockMaximumAmount=-1
Ammo.Secondary.Shared=false
Ammo.Secondary.Shared.Group=-1
Ammo.Secondary.Offset=0,0
```

### Secondary pip scale
Second pip row; `PipScale` accepts a comma list (primary,secondary).

```ini
[SOMETECHNO]
PipScale=Ammo,Passengers
PipScale2=            ; None | Ammo | Tiberium | Passengers | Power | MindControl | Ammo2
PassengersPipSize=
PassengersPipOffset=10,0
```

### Empty image **(already documented)**
Alternate image for empty spawners / out-of-ammo units.

```ini
[SOMETECHNO]
Image.Empty=
WaterImage.Empty=
```

### Infantry fire on the move
Infantry fire during normal Move/Patrol.

```ini
[SOMEINFANTRY]
AttackMove=false
AttackMove.Face=false
AttackMove.FireMulti=0.8
AttackMove.FireRate=0.8
```

```ini
[SOMEINFANTRY]        ; artmd.ini
WalkFire=-1
WalkFire.Rate=1.0
```

### Move position indicator
Dashed line to the pathfinding destination when the target is out of range.

```ini
[AudioVisual]
MovePositionIndicator=yes
MovePositionIndicatorColor=255,255,0
[General]
MovePositionIndicator.ShowEnemy=no
```

### Urban combat
`UrbanCombat=yes` infantry enter enemy garrisons to fight indoor battles.

```ini
[General]
UrbanCombat.AttackBuff=1.0
UrbanCombat.Rate=1.5
UrbanCombat.OutsideDamageMultiplier=0.3
UrbanCombat.DefOutsideMulti=1.0
UrbanCombat.AttOutsideMulti=0.9
[SOMEINFANTRY]
UrbanCombat=no
```

### Battle Points
Per-house resource from kills, usable by superweapons.

```ini
[General]
BattlePoints=
BattlePoints.DefaultValue=
BattlePoints.DefaultFriendlyValue=
[SOMETECHNO]
BattlePoints=
[HouseType]
BattlePoints=no
BattlePoints.CanUseStandardPoints=no
[SuperWeaponType]
BattlePoints.Amount=0
```

```ini
[ToolTips]            ; uimd.ini
BattlePoints.Label=
[Sidebar]
BattlePointsSidebar.Label=
BattlePointsSidebar.Label.InvertPosition=no
```

### BattlePoints production **(already documented)**
Buildings produce Battle Points over time.

```ini
[SOMEBUILDING]
BattlePoints.ProduceStartup=0
BattlePoints.ProduceAmount=0
BattlePoints.ProduceDelay=0
BattlePoints.ProduceDisplay=false
```

### Improve occupants firing logic **(already documented)**
```ini
[CombatDamage]
FixOccupyFire=false
UseGlobalOccupyRange=true
```

### Remove shroud globally
Clears the whole-map shroud at scenario start (fog untouched).

```ini
[General]
RemoveShroudGlobally=no
```

### Missile spawn attack cell
Restores legacy spawned-missile targeting of the target object instead of its
cell.

```ini
[General]
MissileSpawnAttackCell=no
```

### BetterPerformance
Throttles interceptor/shield scans and reuses pip buffers.

```ini
[General]
BetterPerformance=no
```

---

## 5. Types, Overlays & Smudges

### Smudge stacking & SmudgeType extensions
```ini
[General]
SmudgeCapacity=1
[SMUDGETYPE]
Unclearable=yes
CustomPalette=
DrawObject=no
```

### Animation CreateOverlay / CreateSmudge
```ini
[ANIMTYPE]            ; artmd.ini
CreateOverlay=TIB01
CreateOverlay.Overwrite=no
CreateOverlay.Frame=-1
CreateSmudge=CRATER,BURN01
CreateSmudge.Overwrite=no
```

### Overlay buildable-on / palette / ignore-object
```ini
[General]
Tiberium.CanBeBuiltOn=no
Wall.CanBeBuiltOn=no
Rock.CanBeBuiltOn=no
CanBeBuiltOnOverlay.Remove=yes
[TIB01]
CanBeBuiltOn=
CanBeBuiltOn.Remove=
CustomPalette=
IgnoreObject=no
```

---

## 6. Warheads & Weapons

### RemoveShroudOnly
```ini
[WarheadType]
RemoveShroudOnly=0
```

### Transport penetration
```ini
[WarheadType]
PenetratesTransport.Level=0
PenetratesTransport.PassThrough=1.0
PenetratesTransport.FatalRate=0.0
PenetratesTransport.DamageMultiplier=1.0
PenetratesTransport.DamageAll=no
PenetratesTransport.CleanSound=-1
[SOMETECHNO]
PenetratesTransport.Level=
PenetratesTransport.PassThroughMultiplier=1.0
PenetratesTransport.FatalRateMultiplier=1.0
PenetratesTransport.DamageMultiplier=1.0
```

### Ammo modifier
```ini
[WarheadType]
Ammo=0
```

### Directional versus
```ini
[WarheadType]
Versus.<TechnoType>.FrontMultiplier=1.0
Versus.<TechnoType>.SideMultiplier=1.0
Versus.<TechnoType>.RearMultiplier=1.0
```

### UC.PassThrough.Ignore
```ini
[WarheadType]
UC.PassThrough.Ignore=no
```

---

## 7. Superweapons

### Charge system
```ini
[SuperWeaponType]
Charge=-1
Charge.AtOnce=no
Charge.Recharge.All=no
Charge.FullTimer=no
Charge.Chargers=
Charge.ChargerTimes=
```

### LimboDelivery.Delay
```ini
[SuperWeaponType]
LimboDelivery.Delay=0
```

### SpyPlane / ParaDrop spawn points
```ini
[SuperWeaponType]
SpyPlane.Type=
ParaDrop.Aircraft=
SpawnPoints=
SpawnPoints.Delay=
```

---

## 8. AI & Scripting

### Aircraft team-script movement uses Patrol
Aircraft with ammo at a movement-script endpoint are sent on `Patrol` instead of
`Move`. No INI keys.

### Leaving-team member off-map cleanup
Team members beyond the map boundary are culled (30-frame grace; aircraft only
while the team is leaving). No INI keys.

### Extended aircraft missions enhancements **(base already documented)**
Area-guard/guard aircraft hover and re-acquire targets, team patrol/attack-move
search, return-to-archive after target death, and no untethering on area guard.

```ini
[General]
ExtendedAircraftMissions=false
```

### TAction 125 "Build At" fix
Created building plays buildup animation; `Param4` sets `ShouldRebuild`.

---

## 9. Performance & Miscellaneous

### CnCNet mission result file
Writes `Client\MissionResult.ini` on win/lose for client achievements, including
a `[Globals]` dump of set variables on victory.

### INI list corruption diagnostics and repair
Validates engine INI node lists and repairs/skips corrupt map UU data to avoid
crashes. Debug-log only.

---

## 10. Stability Fixes

- **Garrison occupant / broken capture crash fixes** – removes dead occupant
  pointers and clamps the firing-occupant index; guards building capture against
  a NULL new owner.
- **Aircraft idle, enter and helipad-loss fixes** – safer `EnterIdleMode`, fixed
  `Enter` mission for non-Carryall, forced takeoff when the helipad is destroyed.
- **Reverse-aware crushing tilt** – vehicles leaning backwards while reversing
  over a crush victim.
- **Pathfinding failure throttle** – see section 1.

---

## 11. Removed relative to build 48

- **Quicksave hotkey command** was removed; campaign persistence on exit is now
  handled by *Continue game on exit*.
- **FPS config `ShowFPS`** and **`SaveGameOnScenarioStart`** are read but
  hard-forced off in code (the FPS overlay is hotkey-driven; the scenario-start
  autosave caused a multi-second freeze).

---

## Full INI key index

### `rulesmd.ini` → `[General]`
`AIDirectionalArmor`, `AIDirectionalArmor.Chance`, `AIDirectionalArmor.Delay`,
`AttachmentTopLayerMinHeight`, `AttachmentUndergroundLayerMaxHeight`,
`BattlePoints`, `BattlePoints.DefaultValue`, `BattlePoints.DefaultFriendlyValue`,
`BetterPerformance`, `CanBeBuiltOnOverlay.Remove`,
`DisableLoadGame.Difficulty`, `DisableSaveGame.Difficulty`,
`ExtendedAircraftMissions`, `LockGameSpeed.Difficulty`,
`MissileSpawnAttackCell`, `MovePositionIndicator.ShowEnemy`,
`RemoveShroudGlobally`, `ReverseMove`, `Rock.CanBeBuiltOn`, `SmudgeCapacity`,
`SmoothMove`, `TA.VirtualUnit`, `TacticalGear`, `TacticalPause`,
`TacticalPause.BlockActions.Difficulty`, `Tiberium.CanBeBuiltOn`,
`UrbanCombat.AttackBuff`, `UrbanCombat.AttOutsideMulti`,
`UrbanCombat.DefOutsideMulti`, `UrbanCombat.OutsideDamageMultiplier`,
`UrbanCombat.Rate`, `Wall.CanBeBuiltOn`

### `rulesmd.ini` → `[CombatDamage]`
`Armor.FrontMultiplier`, `Armor.SideMultiplier`, `Armor.RearMultiplier`,
`FixOccupyFire`, `PenetratesTransport.Level`, `UseGlobalOccupyRange`

### `rulesmd.ini` → `[AudioVisual]`
`MovePositionIndicator`, `MovePositionIndicatorColor`

### `rulesmd.ini` → TechnoType sections
`Ammo.Secondary*`, `Attachment<n>.*`, `AdvancedDrive*`, `AIDirectionalArmor*`,
`Armor.*Multiplier`, `AttackMove*`, `BattlePoints`, `Image.Empty`,
`PassengersPipOffset`, `PassengersPipSize`, `PenetratesTransport*`, `PipScale`,
`PipScale2`, `ShowMovePositionIndicator`, `SmoothMove`, `UrbanCombat`,
`WaterImage.Empty`

### `rulesmd.ini` → other sections
`[AttachmentTypes]`, `[ArtilleryTypes]`, `[BomberTypes]`,
`[HouseType] BattlePoints` / `BattlePoints.CanUseStandardPoints`,
`[SuperWeaponType] BattlePoints.Amount` / `Charge*` / `LimboDelivery.Delay` /
`SpyPlane.Type` / `ParaDrop.Aircraft` / `SpawnPoints*`,
`[OverlayType] CanBeBuiltOn` / `CanBeBuiltOn.Remove` / `CustomPalette` /
`IgnoreObject`, `[SmudgeType] Unclearable` / `CustomPalette` / `DrawObject`,
`[AnimType] CreateOverlay*` / `CreateSmudge*`

### `RA2MD.INI` → `[Phobos]`
`ContinueGameOnExit`, `Formation*`, `MessageApplyHoverState`,
`MessageDisplayInCenter*`, `RightClickCommands*`,
`ShowGameTime*`, `TacticalZoom*`, `TextScale*`

### `uimd.ini`
`[TacticalZoom] *`, `[ToolTips] BattlePoints.Label`,
`[Sidebar] BattlePointsSidebar*`
