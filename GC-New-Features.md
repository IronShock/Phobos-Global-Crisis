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

### Chained weapon fallback (`Next`)
A weapon can name a `Next=` weapon to fall back to while it is reloading / unable to
fire. Each weapon tracks its own `ROF` cooldown independently, so several weapons can
reload concurrently. Chained weapons must be declared as slots on the TechnoType
(`Weapon1`, `Weapon2`, ... + `WeaponCount` + `MultiWeapon=yes`).

```ini
[SOMEWEAPON]                  ; WeaponType
Next=                         ; WeaponType - weapon to use when this one cannot fire

[SOMETECHNO]                  ; TechnoType
Weapon1=SOMEWEAPON
Weapon2=SOMEWEAPON2
Weapon3=SOMEWEAPON3
WeaponCount=3
MultiWeapon=yes
```

### Autonomous extra weapons (`ExtraFire`)
A weapon can declare additional weapons (`ExtraFire0`, `ExtraFire1`, ...) that the firer
fires autonomously, each on its own `ROF`, alongside the weapon that declared them. Each
extra weapon can override its firing position (`FLH`), pick its own target
(`OwnTarget`/`AutoTarget`, with explicit attack orders always taking priority and
neutrals never auto-acquired), use its own range, optionally consume ammo (primary or
`Ammo.Secondary`), apply its own damage multiplier and firing chance, and optionally
require the target to be within the parent weapon's range (`CheckParentRange`).
`Burst`/`Burst.Delays` are supported. Reload timers are frozen while the firer is
paralysed or under EMP, and are unaffected by veterancy changes.

An entry can also be given a **visible model** with `Image=<UnitType ID>`: the referenced
vehicle's art is drawn at that entry's firing position, and with `Turret` (which defaults to
`OwnTarget`) the mount gets its own facing and independently tracks and aims at its own
target, even while reloading. Frame and rotation come from
the referenced UnitType's own art config, so no frame keys are needed. The model position is
specified the same way as an `Attachment` mount: `Image.FLH` / `Image.IsOnTurret` override
the firing `FLH` / `OnTurret` for the model alone (both default to the firing values), and
`Image.RotationAdjust` applies an `AttachmentN.RotationAdjust`-style angle to the art.
By default (`Image.Muzzle=yes`) the weapon fires from the mounted model's own muzzle - read
from the mount unit itself for voxels, or from the model type's art (`PrimaryFireFLH` +
`TurretOffset`) for SHPs - so a mounted gun shoots out of its own barrel and no `FLH` needs
to be written. `Image.Muzzle=no` fires from the parent's muzzle instead.
`Image.Offset` is a final screen-pixel nudge and `Image.HouseColor` applies the owner's
house colour.

```ini
[SOMEWEAPON]                       ; WeaponType
ExtraFire0=SOMEOTHERWEAPON         ; WeaponType - fires independently alongside this weapon
ExtraFire0.FLH=0,0,0               ; firing position (defaults to parent slot FLH)
ExtraFire0.OnTurret=yes            ; FLH rotates with turret (default yes)
ExtraFire0.OwnTarget=no            ; pick nearest valid enemy independently
ExtraFire0.AutoTarget=no           ; OwnTarget=no only: acquire a target when the firer has none
ExtraFire0.Range=                  ; acquisition range in cells (default: weapon Range)
ExtraFire0.UseAmmo=no              ; each shot consumes 1 unit Ammo
ExtraFire0.UseAmmo2=no             ; each shot consumes 1 unit Ammo.Secondary instead
ExtraFire0.CheckParentRange=no     ; target must be within parent weapon range (beats OwnTarget)
ExtraFire0.CheckFacing=yes         ; turret/body must actually face the target (beats OwnTarget)
ExtraFire0.FacingTolerance=512     ; CheckFacing tolerance in raw direction units (~2.8 degrees)
ExtraFire0.TurnToTarget=no         ; nudge the turret towards the target while waiting to face it
ExtraFire0.OnlyWhenParentFires=no  ; fire only in the window the parent slot just fired in
ExtraFire0.DamageMultiplier=1.0    ; damage multiplier on top of the firer's firepower
ExtraFire0.Chance=1.0              ; 0..1 chance to fire when every other gate allows it
ExtraFire0.Image=                  ; UnitType ID - model drawn at this entry's firing position
ExtraFire0.Image.FLH=              ; integer - model position, defaults to ExtraFire0.FLH
ExtraFire0.Image.IsOnTurret=       ; boolean - model FLH rotates with the turret, defaults to ExtraFire0.OnTurret
ExtraFire0.Image.RotationAdjust=0  ; integer - art angle offset, 1/256 of a circle per unit
ExtraFire0.Image.Muzzle=yes        ; boolean - fire from the mounted model's own muzzle (default yes)
ExtraFire0.Image.Offset=0,0        ; screen pixel offset for the drawn model
ExtraFire0.Image.HouseColor=yes    ; remap the drawn model to the owner's house colour (default yes)
ExtraFire0.Turret=no               ; the mount keeps its own facing (independent turret)
ExtraFire0.Turret.ROT=             ; turn rate, same units as the game's ROT (default the type's ROT)
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

## 12. MO_ExperienceEnhancements port

Several quality-of-life, queue and observer modules from
**MO_ExperienceEnhancements** by **MagicShiba** are ported into this build. The
upstream project is a standalone Ares plugin for Mental Omega (3.3.6); here the
modules are rewritten in Phobos' `DEFINE_HOOK` / INI-config style while keeping
the original hook and patch addresses (both projects target the same YR 1.001
`gamemd.exe`).

Ported so far (see `src/MOEE/`):

- **Observer fix** (`MOEE.Observer`) – multi-observer support (aligns the local
  `CurrentPlayer` to the global Observer house) and observer panel ordering
  (human houses first, AI after, no observer slots).
- **MegaMissionList capacity** (`MOEE.MegaListCap`) – raises
  `EventClass::MegaMissionList` from 256 to 4096 entries (single player only).
- **Waypoint limits** (`MOEE.WaypointCap`) – raises the single-player waypoint
  limits (vanilla 16 per route / 128 planning) to a configurable value; the
  original branch/planning checks are reproduced through Phobos hooks and an
  in-place immediate patch, multiplayer always keeps vanilla behaviour.
- **Custom attack-move hotkey** (`MOEE.AttackMove`) – any hotkey can issue
  `Mission::AttackMove` (vanilla Ctrl+Shift+LMB) through the standard
  `EventClass::OutList` path; holding Z queues a planning waypoint.
- **Repair simplification** (`MOEE.RepairAll`) – Ctrl+repair / double-click
  repair / automatic repair of all damaged own buildings, reusing the vanilla
  wrench trigger points and issuing standard `EventType::Repair` events.
- **Place scatter** (`MOEE.PlaceScatter`) – pushes your own ground units out of
  the footprint when placing a building (on click, or continuously while the
  ghost follows the cursor), issuing standard `Mission::Move` events.
- **Order throttle** (`MOEE.OrderThrottle`) – removes the single-player 128-event
  per-frame OutList limit by writing over-budget orders straight into DoList
  (capacity 16384) at the three vanilla "OutList full" branches. Multiplayer is
  bit-identical to vanilla. This is what makes large groups and
  `MOEE.WaypointCap=0` (unlimited waypoints) safe in single player.
- **Tree chop** (`MOEE.TreeChop`) – Ctrl+Z+Middle: a range box follows the
  cursor and, on release, every tree inside is force-attacked, distributed
  round-robin across the selected ground units.
- **Fuzzy attack** (`MOEE.FuzzyAttack`) – Ctrl+Middle: fuzzy force-attack for
  hard-to-click targets. Scans a range box (nearest ring first), groups the
  selection "`SquadSize` to one" and draws red unit→target lines while held.
- **Smart order** (`MOEE.SmartOrder`) – bare Middle click: context-sensitive
  order (attack / enter / capture, decided per unit by the engine), or a
  point-scatter around the clicked cell on empty ground (infantry pack 3 per
  cell). Green preview lines while held.
- **Patrol circle** (`MOEE.PatrolCircle`) – Z+Middle: places a patrol ring for
  the whole selection (tap = square, drag = rectangle). Every waypoint is
  issued as a planning MegaMission, then connected (`PlanConnect`) and committed
  (`PlanCommit`).
- **Smart area** (`MOEE.SmartArea`) – Shift+Middle: area-context order. For each
  unit picks the first target the engine allows (attack / enter / capture /
  sabotage) with a per-target `SquadSize` quota; never issues move orders.
- **Deploy preview** (`MOEE.DeployPreview`) – Middle-held footprint preview for a
  base-deployable unit; on release it is ordered there and deployed on arrival.
  Claims the bare middle button while such a unit is selected (SmartOrder yields).

### 12.1 UI / map (second group)

- **Map centering** (`MOEE.MapCenter`) – replaces the engine's `Clamp_Center`
  (`0x6D8640`). When the viewport is larger than the map the camera is pinned to
  the map centre instead of toggling between inverted bounds, fixing the
  left/right jitter at high resolutions.
- **Weapon range** (`MOEE.WeaponRange`) – Ctrl + a single selected unit draws the
  current weapon's maximum range (and minimum range when non-zero) as an ellipse.
- **Ground grid** (`MOEE.GridView`) – toggleable tactical ground grid via a
  hotkey; cells carrying a building or terrain are drawn red.
- **Zoom scale bar** (`MOEE.ScaleBar`) – bottom-bar scale with ticks from
  `MinZoom` to `MaxZoom`; highlights the current level and sets `ZoomManager`'s
  zoom when a tick is clicked. Drives the existing ZoomManager rather than a
  second zoom pipeline.

New `RA2MD.ini` → `[Phobos]` keys:

```ini
MOEE.Observer=true          ; observer panel fix
MOEE.MegaListCap=true       ; MegaMissionList 256 -> 4096 (single player only)
MOEE.WaypointCap=128        ; single-player waypoint cap (0 = unlimited, max 4096; MP always vanilla 16/128)
MOEE.AttackMove=X2          ; custom attack-move hotkey ("0"/"None" disables; e.g. X2, Middle, Ctrl+Z+Middle)
MOEE.RepairAll=0            ; repair simplification: 0=off, 1=manual (Ctrl/double-click), 2=automatic
MOEE.RepairAll.Interval=30  ; automatic rescan interval (frames, 5..600)
MOEE.RepairAll.Batch=24     ; max Repair events per scan (1..96)
MOEE.RepairAll.DoubleClick=true   ; also trigger on double-clicking the wrench
MOEE.RepairAll.DblFrames=12 ; double-click window (frames, 3..60)
MOEE.PlaceScatter=1         ; clear units out of the footprint: 0=off, 1=on click, 2=also chase the ghost
MOEE.PlaceScatter.Interval=30   ; Mode 2 rescan interval (frames, 5..300)
MOEE.PlaceScatter.Batch=16      ; max Move events per trigger (1..64)
MOEE.PlaceScatter.Dist=2        ; extra cells to push past the footprint (1..4)
MOEE.OrderThrottle=1            ; single-player order cap removal (0=off, 1=on); multiplayer always vanilla
MOEE.TreeChop=Ctrl+Z+Middle     ; quick tree chopping hotkey ("None"/"0" disables)
MOEE.TreeChop.Range=4           ; scan box size in cells (2..16)
MOEE.FuzzyAttack=Ctrl+Middle    ; fuzzy force-attack hotkey ("None"/"0" disables)
MOEE.FuzzyAttack.Range=4        ; scan box size in cells (2..16)
MOEE.FuzzyAttack.SquadSize=4    ; units per target (>SquadSize splits evenly; 2..16)
MOEE.SmartOrder=Middle          ; smart middle-click hotkey ("None"/"0" disables)
MOEE.SmartOrder.NodeStep=2      ; line-drag sampling step (reserved; 1..8)
MOEE.PatrolCircle=Z+Middle      ; patrol ring hotkey ("None"/"0" disables)
MOEE.PatrolCircle.Size=4        ; ring size in cells (2..16)
MOEE.PatrolCircle.Commit=true   ; commit the plan after connecting the ring
MOEE.SmartArea=Shift+Middle     ; area-context order hotkey ("None"/"0" disables)
MOEE.SmartArea.Range=4          ; scan box size in cells (2..16)
MOEE.SmartArea.SquadSize=4      ; units per target per pass (1..16)
MOEE.DeployPreview=true         ; base-deploy footprint preview + auto deploy
MOEE.DeployPreview.AutoDeploy=true   ; deploy automatically once the unit arrives
MOEE.DeployPreview.Timeout=675  ; arrival tracking timeout (frames, 30..4500)
MOEE.DeployPreview.Key=Middle   ; trigger key for the preview
MOEE.MapCenter=true             ; pin the camera to the map centre when the viewport is larger
MOEE.WeaponRange=true           ; Ctrl + single unit: draw the weapon range ellipse
MOEE.Grid=Ctrl+G                ; toggle the tactical ground grid ("None"/"0" disables)
MOEE.Grid.Radius=20             ; grid half-window in cells around the camera (8..40)
MOEE.ScaleBar=false             ; bottom-bar zoom scale bar (requires TacticalZoom)
MOEE.ScaleBar.X=40              ; right margin from the sidebar (pixels)
MOEE.ScaleBar.Y=1               ; bottom margin (pixels)
MOEE.ScaleBar.Len=250           ; bar length (pixels, 80..2000)
```

### Credits / 鸣谢

Ported from **[MagicShiba/MO_ExperienceEnhancements](https://github.com/MagicShiba/MO_ExperienceEnhancements)**
（MO_ExperienceEnhancements · 心灵终结体验优化拓展）.
All credit for the original design, reverse-engineering and implementations
goes to **MagicShiba** and the upstream contributors. Please support and check
out the original project.

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
`[AnimType] CreateOverlay*` / `CreateSmudge*`, `[WeaponType] Next` / `ExtraFire*`

### `RA2MD.INI` → `[Phobos]`
`ContinueGameOnExit`, `Formation*`, `MessageApplyHoverState`,
`MessageDisplayInCenter*`, `MOEE.*` (ported from MO_ExperienceEnhancements),
`RightClickCommands*`, `ShowGameTime*`, `TacticalZoom*`, `TextScale*`

### `uimd.ini`
`[TacticalZoom] *`, `[ToolTips] BattlePoints.Label`,
`[Sidebar] BattlePointsSidebar*`
