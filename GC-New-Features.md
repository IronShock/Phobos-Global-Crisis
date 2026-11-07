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

### Enhanced drive acceleration
Time-based acceleration/deceleration for `Drive` and `AdvancedDrive`, with easing curves, calculated braking distance, minimum speed and dynamic terrain/turn/slope/load/health factors. `Acceleration.Mode=enhanced` must be set explicitly (requires `Accelerates=yes`, inherited values count).

- `Acceleration.Curve` / `Deceleration.Curve` are string enums (`linear`, `ease-in`, `ease-out`, `smoothstep`), defaulting to `linear`.
- A missing `Acceleration.Time` / `Deceleration.Time` defaults to 60 / 30 frames in enhanced mode; the legacy `AccelerationFactor` / `DeaccelerationFactor` values are not used.
- A stationary unit starting to move always ramps from 0%, even if `SpeedPercentage` still carries a stale value. Small target changes do not restart the ramp.
- `InitialSpeed` is the speed fraction used when a stationary unit starts moving (default 0); `MinimumSpeed` is the braking floor, not the starting speed. `BrakeDistance=-1` calculates the needed braking distance from current speed and deceleration; `>0` overrides it in leptons; `0` disables early braking. `StopDistance` is the final approach window (target becomes `MinimumSpeed`; the vanilla locomotor stops the unit) and `MinimumSpeed` replaces the vanilla 0.3 floor.
- `TerrainSpeed.*` and `SlopeSpeedFactor` are additional multipliers on top of the vanilla `GroundType` cost. `LoadSpeedFactor`, `HealthSpeedFactor` / `HealthSpeedThreshold`, `TurnSpeedFactor`, crushing and sinking are applied before the ramp.
- The first 120 frames of each ramp are logged unconditionally (start line plus every 10 frames), including `before`, `after`, `target` and `start`; `Debug.GCDiagnostics=true` keeps logging after that. The INI load line shows the effective times. Starting a new move order resets the ramp to 0%.
- Only `Drive` and `AdvancedDrive` are supported. The ramp state is serialized, so older savegames are incompatible.

```ini
[SOMEVEHICLE]
Acceleration.Mode=enhanced
Acceleration.Time=60
Deceleration.Time=30
Acceleration.Curve=ease-in      ; string enum: linear | ease-in | ease-out | smoothstep
Deceleration.Curve=ease-out     ; string enum: linear | ease-in | ease-out | smoothstep
InitialSpeed=0.0
MinimumSpeed=0.05
BrakeDistance=-1
BrakeDistance.Multiplier=1.0
StopDistance=16
TurnSpeedFactor=1.0
SlopeSpeedFactor=1.0
LoadSpeedFactor=1.0
HealthSpeedFactor=1.0
HealthSpeedThreshold=0.5
TerrainSpeed.Road=1.0
TerrainSpeed.Rough=1.0
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

### Tactical map overlay
Company of Heroes-style enlarged minimap. The hotkey (`Tactical Map`, default
`Ctrl+M`, configurable in Options -> Keyboard -> Interface) opens a full-screen
overlay that shows the sidebar minimap scaled up in the middle of the tactical
viewport, with the background dimmed. The content is copied straight from the
minimap, so terrain, unit dots, the view rectangle, radar events and the offline
state always match the sidebar by construction. Left or right clicking the
enlarged map moves the camera to that spot, exactly like clicking the minimap;
no orders are issued. While it is open, map clicks and HUD buttons are blocked so
nothing underneath can be triggered by accident. `ESC` closes it again.

The overlay is client-side only (safe in multiplayer), is disabled in Cinema mode
(there is no minimap to copy) and follows resolution changes automatically. While
it is open the camera is pinned, so edge scrolling (and keyboard scrolling) can no
longer drag the view away; clicking the enlarged map is the only way to move it.

```ini
[TacticalMap.ini]               ; next to gamemd.exe
[Settings]
Enabled=true
Scale=0                         ; 0 = auto-fit, 1-8 = fixed multiplier
FitPercent=90                   ; auto-fit limit, percent of the viewport
Filter=nearest                  ; nearest|linear (linear = 2x2 box filter)
BackdropOpacity=60              ; 0-100, dims the viewport behind the map
BorderColor=220,220,220
```

Settings and hotkeys live in `TacticalMap.ini` because the game rewrites
`RA2MD.INI` and would drop keys it does not know.

### Tactical map annotation tools
While the tactical map overlay is open, a vertical tool panel is shown to the
right of the enlarged map:

- **Camera** (default): left/right click moves the view, exactly like the minimap.
- **Brush**: hold the left button to draw freehand on the map. Annotations are
  stored in map-normalised space, so they stay glued to the same map position when
  the overlay is closed and reopened or the resolution changes.
- **Eraser**: hold the left button to erase with a round brush.
- **Ruler**: drag to measure a straight-line distance in game cells; the result is
  kept as a line with a label. Right click removes the most recent measurement.
- **Text**: click to open an inline editor at that spot and type (Chinese input
  methods work); `Enter` commits, `Esc` cancels. Text colour follows the current
  brush colour and its size follows the brush width (12/14/16/18/20/22/26 px).
  Clicking an existing label with the text tool deletes it.
- **Marker**: click to drop an inverted triangle whose tip sits on the clicked
  point, using the current brush colour; clicking an existing marker deletes it.
- **Colour**: opens an eight-colour palette (hotkey cycles instead).
- **Width**: opens a width picker (hotkey cycles instead): 2/4/6/8/10/12/16 px.
- **Clear**: removes all drawings, measurements, labels and markers (undoable).
- **Undo**: `Ctrl+Z` by default, 50 steps covering strokes, labels, markers, ruler
  lines and "clear".

While the tactical map is open it owns the keyboard: every game hotkey is disabled
and only the tactical map hotkeys below are active. They live in `TacticalMap.ini`
next to the executable, because the game rewrites `RA2MD.INI` and would drop them.

```ini
[TacticalMap.ini]
[Settings]
Enabled=true
Scale=0
FitPercent=90
Filter=nearest
BackdropOpacity=60
BorderColor=220,220,220
Toolbar=true
Brush.Color=255,240,80
Brush.Width=2
Eraser.Radius=12
Ruler.Color=120,220,255

[Multiplayer]
ShareWithTeam=0

[Hotkeys]
Camera=Ctrl+V
Brush=Ctrl+B
Eraser=Ctrl+E
Ruler=Ctrl+R
Text=Ctrl+T
Marker=Ctrl+P
Color=Ctrl+C
Width=Ctrl+W
Clear=Ctrl+Delete
Undo=Ctrl+Z
Close=Ctrl+M
```

Hotkey syntax is `Ctrl`/`Shift`/`Alt` plus one key (letters, digits, `F1`-`F12`,
`Delete`, `Space`, `Enter`, `Tab`, arrows). Markers use the current brush colour and
their size follows the brush width. Ruler labels are centred on the measured line.

`ESC` closes the overlay (or only cancels the text editor while typing).
Annotations are kept until a new scenario starts, are client-side only, and never
write game state, so they are safe in multiplayer.

#### Sharing annotations with allies
With `[Multiplayer] ShareWithTeam=1` (default off) a multiplayer game shares the
whole annotation layer with your allies in real time: brush/eraser strokes (sent
while the button is held, not only on release), labels, markers, ruler lines,
"clear" and undo. Everything is broadcast through the engine's own event queue as
a private event type, so it travels on the same reliable, frame-ordered path as
orders; the handler only ever touches drawing state, so it cannot desync the
simulation. Only allies apply incoming annotations, observers never send or
receive them, and the sender's own echo is ignored. Annotations keep their
original colour and width regardless of who is looking. If a packet is missed the
annotation layer simply differs visually for that one element - game state is
never involved.

```ini
[Phobos]                        ; RA2MD.INI (in addition to the keys above)
TacticalMap.Toolbar=true
TacticalMap.Brush.Color=255,240,80
TacticalMap.Brush.Width=3
TacticalMap.Eraser.Radius=12
TacticalMap.Ruler.Color=120,220,255
```

**Localisation.** The overlay has one built-in text string, the measurement unit
suffix. It is looked up through the game string table and defaults to English, so
it reads correctly even without a CSF entry:

| Key | English | 简体中文 |
| --- | --- | --- |
| `TXT_TACTICAL_MAP` | Tactical Map | 战术地图 |
| `TXT_TACTICAL_MAP_DESC` | Open or close the enlarged minimap overlay. Click it to move the camera. | 打开或关闭放大的小地图覆盖层。点击地图可移动视角。 |
| `TXT_TACTICAL_MAP_CELLS` | cells | 格 |

The two `TXT_TACTICAL_MAP*` keys label the keyboard command (Options → Keyboard →
Interface), and `TXT_TACTICAL_MAP_CELLS` is the unit shown next to measurement
labels. They are shipped in `Resources/Translations/<lang>/stringtable01.csf`
(`en` and `zh-Hans`); `TXT_TACTICAL_PAUSE` / `_DESC` follow the same pattern for
the tactical time stop command.

### FPS counter / frame-time monitor
Hotkey ("New FPS Counter", Options -> Keyboard -> Interface) cycles through
`Off -> Simple -> Detailed -> Off`.

- Simple: FPS and current frame time.
- Detailed: 1% low / max frame time, Logic vs Render split, building
  draw/AI time, object counts, zoom, and a 60-frame bar graph.

`ShowFPS=yes` starts the overlay in Simple mode; the hotkey still cycles it.
The overlay is client-side only (no simulation or sync impact).

```ini
[Phobos]                     ; RA2MD.INI
ShowFPS=yes

; Frame-time probes and CSV log for profiling (default off).
Debug.PerfProbe=no
Debug.PerfLog=no
```

`Debug.PerfProbe` enables the QPC probes on `BuildingClass::Draw`,
`BuildingClass::AI` and `TacticalClass::Render`. `Debug.PerfLog` writes
`debug\perf.csv` every 300 frames (FPS, frame/logic/render ms, building draw
and AI time, object counts, zoom, camera position).

### LoadBar
A self-contained, client-side progress bar drawn above technos. It covers
time-based states (AttachEffect duration, weapon cooldown, reload, superweapon
recharge, spawn/rearm timers) and value-based states (health, shield, ammo,
passengers, tiberium, experience, factory progress).

- Independent feature: does not extend `DigitalDisplayTypeClass` or
  `AttachEffectTypeClass`, adds no serialized fields and never touches the save
  stream. Old saves load unchanged; new saves contain no LoadBar data.
- Bars are defined once in `[LoadBarTypes]`, assigned per TechnoType ID in
  `[LoadBarAssignments]`, or attached to an AttachEffect type in
  `[LoadBarAttachEffects]`.
- `Fill=remaining` is for durations that shrink (AE time); `Fill=elapsed` is for
  cooldowns that fill up as the weapon becomes ready.
- Anchors, automatic stacking, colours, segmentation, optional text,
  per-house/per-state visibility and optional zoom scaling (`ScaleWithZoom`)
  are all INI-configurable.
- `Color.Gradient=yes` blends the fill colour instead of switching at
  thresholds. Define any number of stops with `Color.Gradient.<i>.At` /
  `Color.Gradient.<i>.Color`; consecutive pairs are scanned automatically until
  one is missing, so there is no stop-count key. Explicit stops always apply,
  even with `Color.Gradient=no`; the flag only controls the automatic fallback
  derived from `Color`, `Color.ConditionYellow` and `Color.ConditionRed`. The
  same keys in `[LoadBar]` act as global defaults for every bar.
- `LoadBar.MaxPerFrame` caps the number of bars drawn per frame; the renderer
  early-outs on objects that are not visible on screen.

```ini
[LoadBar]                    ; rulesmd.ini
Enable=true
MaxPerFrame=256
Vehicles.Default=

[LoadBarTypes]
0=GC_AE
1=GC_ROF

[GC_AE]
Source=AttachEffect
MaxEntries=0                 ; one bar per matching AE instance
Fill=remaining
Width=24
Height=3
Color=64,220,64
ShowType=selected,cursorhover

[GC_ROF]
Source=WeaponCooldown
SourceIndex=-1               ; current weapon (-2 = all slots)
Fill=elapsed
Width=22
Height=2
Color=64,200,255

[LoadBarAssignments]
MTNK=GC_ROF

[LoadBarAttachEffects]
SomeDebuffAE=GC_AE
```

See `docs/User-Interface.md` for the full key list.

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

### Passenger enter directions
Restricts which side of a transport vehicle infantry may board from. Directions
are relative to the transport's current body facing (`front`, `frontright`,
`right`, `backright`, `back`, `backleft`, `left`, `frontleft`). Infantry walk to
an allowed adjacent cell and only board after reaching it, so they are never
removed from a distance. If the transport rotates or moves away from the
assigned cell, the infantry is re-routed. Every passenger that reaches the
vehicle in the same frame boards in that frame, allowing a group to board
together; `EnterStaggerFrames` can optionally pace the hand-offs. Unloading uses
the same sides and waits for a valid exit cell instead of ejecting into water
or a wall. `all` is the vanilla default; `none` blocks boarding but still allows
unloading. Only infantry are restricted, and
capacity/`BySize`/amphibious/bridge checks still apply.

```ini
[SOMEVEHICLE]                            ; VehicleType, transport
Passengers.EnterDirections=all           ; all|none|comma-separated directions
Passengers.EnterStaggerFrames=0          ; frames between hand-offs; 0 = same-frame/parallel
Passengers.UnloadStaggerFrames=2         ; frames between exiting passengers; 0 = instant
; Passengers.EnterDirections=back
; Passengers.EnterDirections=back,backleft,backright
```

### Per-weapon ammo pools
Every weapon slot can have its own ammo pool instead of draining the unit's `Ammo`. `Ammo=0` is unlimited, an unset weapon `Ammo` falls back to the unit `Ammo`, and per-slot overrides (`Weapon0.Ammo.*`, `Weapon1.Ammo.*`, ...) take priority. Elite weapons share their slot pool. Pools reload independently, and `Ammo.Cost` / `Ammo.PerBurst` control how a charge is paid for. Aircraft follow the same per-shot/per-burst rules, return when every slot is dry, and rearm at airfields.

```ini
[SOMEWEAPON]                         ; WeaponType
Ammo=6
Ammo.Initial=6
Ammo.Reload=120
Ammo.Reload.Empty=600
Ammo.Reload.Amount=1
Ammo.Cost=1
Ammo.PerBurst=no

[SOMETECHNO]                         ; per-slot override
Weapon0.Ammo=8
Weapon0.Ammo.Initial=4
Weapon0.Ammo.Reload=90
Weapon0.Ammo.Reload.Empty=180
Weapon0.Ammo.Reload.Amount=1
Weapon0.Ammo.Cost=1
Weapon0.Ammo.PerBurst=no
Weapon0.```

### Secondary pip scale
Second pip row; `PipScale` accepts a comma list (primary,secondary).

```ini
[SOMETECHNO]
PipScale=Ammo,Passengers
PipScale2=            ; None | Ammo | Tiberium | Passengers | Power | MindControl
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
neutrals never auto-acquired), use its own range, consume ammo from its own per-weapon pool or the unit `Ammo`
(following the normal `Ammo` fallback rules), apply its own damage multiplier and firing chance, and optionally
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
ExtraFire0.UseAmmo=no              ; explicit unlimited (omit to follow unit Ammo; ignored if the extra weapon has Ammo)
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
- **`SaveGameOnScenarioStart`** is read but hard-forced off in code (the
  scenario-start autosave caused a multi-second freeze).

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
- **Custom attack-move hotkey** (`MOEE.AttackMove`) – the X2 side button (plus an
  optional keyboard part configured in `KeyboardMD.ini`) issues
  `Mission::AttackMove` (vanilla Ctrl+Shift+LMB) through the standard
  `EventClass::OutList` path; holding Z queues a planning waypoint.
- **Repair simplification** (`MOEE.RepairAll`) – the configured Repair All
  modifier + repair button / double-click repair / automatic repair of all
  damaged own buildings, reusing the vanilla wrench trigger points and issuing
  standard `EventType::Repair` events.
- **Place scatter** (`MOEE.PlaceScatter`) – pushes your own ground units out of
  the footprint when placing a building (on click, or continuously while the
  ghost follows the cursor), issuing standard `Mission::Move` events.
- **Order throttle** (`MOEE.OrderThrottle`) – removes the single-player 128-event
  per-frame OutList limit by writing over-budget orders straight into DoList
  (capacity 16384) at the three vanilla "OutList full" branches. Multiplayer is
  bit-identical to vanilla. This is what makes large groups and
  `MOEE.WaypointCap=0` (unlimited waypoints) safe in single player.
- **Tree chop** (`MOEE.TreeChop`) – hold the configured keyboard part (default
  Ctrl+Z) and the middle button: a range box follows the cursor and, on release,
  every tree inside is force-attacked, distributed round-robin across the
  selected ground units.
- **Fuzzy attack** (`MOEE.FuzzyAttack`) – hold the configured keyboard part
  (default Ctrl) and the middle button: fuzzy force-attack for hard-to-click
  targets. Scans a range box (nearest ring first), groups the selection
  "`SquadSize` to one" and draws red unit→target lines while held.
- **Smart order** (`MOEE.SmartOrder`) – bare Middle click (keyboard part empty by
  default): context-sensitive order (attack / enter / capture, decided per unit
  by the engine), or a point-scatter around the clicked cell on empty ground
  (infantry pack 3 per cell). Green preview lines while held.
- **Patrol circle** (`MOEE.PatrolCircle`) – hold the configured keyboard part
  (default Shift+Z) and the middle button: places a patrol ring for the whole selection
  (tap = square, drag = rectangle). Every waypoint is issued as a planning
  MegaMission, then connected (`PlanConnect`) and committed (`PlanCommit`).
- **Smart area** (`MOEE.SmartArea`) – hold the configured keyboard part (default
  Shift) and the middle button: area-context order. For each unit picks the first
  target the engine allows (attack / enter / capture / sabotage) with a per-target
  `SquadSize` quota; never issues move orders.
- **Deploy preview** (`MOEE.DeployPreview`) – middle-held (keyboard part empty by
  default) footprint preview for a base-deployable unit; on release it is ordered
  there and deployed on arrival. Claims the bare middle button while such a unit
  is selected (SmartOrder yields).

### 12.1 UI / map (second group)

- **Map centering** (`MOEE.MapCenter`) – replaces the engine's `Clamp_Center`
  (`0x6D8640`). When the viewport is larger than the map the camera is pinned to
  the map centre instead of toggling between inverted bounds, fixing the
  left/right jitter at high resolutions.
- **Weapon range** (`MOEE.WeaponRange`) – hold the configured keyboard part
  (default Ctrl+Shift) with a single selected unit to draw the current weapon's
  maximum range in yellow and minimum range (when non-zero) in red as ellipses.

New `RA2MD.ini` → `[Phobos]` keys:

```ini
MOEE.Observer=true          ; observer panel fix
MOEE.MegaListCap=true       ; MegaMissionList 256 -> 4096 (single player only)
MOEE.WaypointCap=128        ; single-player waypoint cap (0 = unlimited, max 4096; MP always vanilla 16/128)
MOEE.RepairAll=0            ; repair simplification: 0=off, 1=manual (configured modifier/double-click), 2=automatic
MOEE.RepairAll.Interval=30  ; automatic rescan interval (frames, 5..600)
MOEE.RepairAll.Batch=24     ; max Repair events per scan (1..96)
MOEE.RepairAll.DoubleClick=true   ; also trigger on double-clicking the wrench
MOEE.RepairAll.DblFrames=12 ; double-click window (frames, 3..60)
MOEE.PlaceScatter=1         ; clear units out of the footprint: 0=off, 1=on click, 2=also chase the ghost
MOEE.PlaceScatter.Interval=30   ; Mode 2 rescan interval (frames, 5..300)
MOEE.PlaceScatter.Batch=16      ; max Move events per trigger (1..64)
MOEE.PlaceScatter.Dist=2        ; extra cells to push past the footprint (1..4)
MOEE.OrderThrottle=1            ; single-player order cap removal (0=off, 1=on); multiplayer always vanilla
MOEE.TreeChop.Range=4           ; scan box size in cells (2..16)
MOEE.FuzzyAttack.Range=4        ; scan box size in cells (2..16)
MOEE.FuzzyAttack.SquadSize=4    ; units per target (>SquadSize splits evenly; 2..16)
MOEE.SmartOrder.NodeStep=2      ; line-drag sampling step (reserved; 1..8)
MOEE.PatrolCircle.Size=4        ; ring size in cells (2..16)
MOEE.PatrolCircle.Commit=true   ; commit the plan after connecting the ring
MOEE.SmartArea.Range=4          ; scan box size in cells (2..16)
MOEE.SmartArea.SquadSize=4      ; units per target per pass (1..16)
MOEE.DeployPreview=true         ; base-deploy footprint preview + auto deploy
MOEE.DeployPreview.AutoDeploy=true   ; deploy automatically once the unit arrives
MOEE.DeployPreview.Timeout=675  ; arrival tracking timeout (frames, 30..4500)
MOEE.MapCenter=true             ; pin the camera to the map centre when the viewport is larger
MOEE.WeaponRange=true           ; draw the weapon range ellipse while the key part is held
```

The keyboard part of each mouse combo lives in `KeyboardMD.ini` → `[Hotkey]`
(it is also editable in-game under `Options → Keyboard`, category `MOEE`). The
mouse button itself is fixed; an empty binding means "mouse button only".

```ini
; Keyboard part only — the mouse button is fixed in code.
MOEE Tree Chop=602          ; Ctrl+Z + middle
MOEE Fuzzy Attack=17        ; Ctrl + middle
MOEE Patrol Circle=346      ; Shift+Z + middle
MOEE Smart Area=16          ; Shift + middle
MOEE Weapon Range=768       ; Ctrl+Shift (hold)
MOEE Repair All=1536        ; Ctrl+Alt (hold)
; MOEE Attack Move / MOEE Smart Order / MOEE Deploy Preview: empty by default
```

### 12.2 Vector text (VecText)

A port of MO_EE's global vector-text takeover: the engine's bitmap glyph
rasterisation is replaced with GDI-rendered TrueType glyphs while every metric,
wrap and alignment path still goes through the engine.

- Hooked at the same five BitFont entry points Phobos' `TextScale` uses, but via
  Syringe hooks with a naked `ret 0x10` takeover stub and plain `return 0`
  fall-through - **no runtime trampolines** (both `GetTextDimension` and
  `BitFont::Blit` are `thiscall` + 4 stack args with a `ret 10h` epilogue,
  verified by disassembling `gamemd.exe`).
- Font selection mirrors the CnCNet client's
  `Resources/Translations/<lang>/Fonts.ini` chain: one face for Latin/digits and
  another for CJK, picked per glyph. Font files are searched in the game folder,
  the client's `Resources/Translations/*/Fonts` and `Resources/Fonts` folders,
  then the system fonts.
- `CjkOnly` (default) takes over **CJK glyphs only**; Latin and digits keep the
  engine's bitmap font.
- **Sizing** follows the engine: the pixel size is calibrated to the engine's own
  glyph ink height (measured from the bitmap font data), and it inherits the
  engine's current line height. Because `TextScale` swaps the engine font per
  target surface, the vector text ends up with exactly the same scope: in-game
  text scales, the sidebar / dialogs / shell do not.
- `GetTextDimension` is taken over to return the **true line count** for wrapped
  text (the engine returns a single line height), so multi-line message
  background boxes cover every line.
- `TextScale::Update()` is called from the takeover paths so its per-call
  bookkeeping (font swap + message-list row pitch) keeps running.
- Extras: dialog-button text is captured and re-laid out (centred in the
  button); the campaign briefing body is redrawn as a block (line pitch,
  tracking, wrapping by ink width, optional double-line border mark); top-strip
  digits can take their own size / vertical nudge.

New `RA2MD.ini` → `[Phobos]` keys:

```ini
MOEE.VecText=true
MOEE.VecText.FontFace=HarmonyOS_Sans_SC_Regular.ttf    ; CJK face
MOEE.VecText.FontFaceLatin=Roboto_SemiCondensed-Medium.ttf
MOEE.VecText.FontFaceBold=HarmonyOS_Sans_SC_Bold.ttf
MOEE.VecText.FontFaceLatinBold=Roboto_SemiCondensed-Bold.ttf
MOEE.VecText.FontPx=0            ; 0 = auto-calibrate to the engine glyph size
MOEE.VecText.Bold=false
MOEE.VecText.DigitTrack=0        ; digit tracking tweak (px, -8..8)
MOEE.VecText.CjkOnly=true        ; only CJK glyphs are vectorised
MOEE.VecText.MenuTake=true       ; replace dialog-button text (centred re-layout)
MOEE.VecText.CreditPx=0          ; top-strip digit size (0 = off; CjkOnly=false only)
MOEE.VecText.CreditDY=0
MOEE.VecText.Brief=true          ; campaign briefing body self-draw
MOEE.VecText.BriefLine=125       ; line pitch, percent of font size
MOEE.VecText.BriefOwn=true       ; 1 = natural advance placement
MOEE.VecText.BriefTrack=0        ; letter spacing (px)
MOEE.VecText.BriefMark=true      ; dark double-line border around the text
MOEE.VecText.BriefWidth=580      ; body width (0 = engine default)
MOEE.VecText.BriefAdv=true       ; 1 = per-glyph advance, 0 = uniform cell width
```

### 12.3 Minimap radar signal markers (`RadarSignal`)

Attaches a custom SHP marker to any `TechnoType` (infantry / vehicle / aircraft /
building) on the minimap, replacing the engine's one-pixel blip. Vanilla
visibility rules are honoured by default, and a marker can be forced visible for
mission targets.

```ini
[GAPOWR]
RadarSignal=yes                  ; master switch, must be yes
RadarSignal.Image=MYICON         ; SHP name; .shp is appended when missing
RadarSignal.Palette=myicon.pal   ; optional, defaults to sidebar.pal; ~~~ = theater suffix
RadarSignal.Frame=0              ; first frame
RadarSignal.FrameCount=1         ; number of frames to cycle, 1 = static
RadarSignal.AnimRate=6           ; logic frames per animation frame
RadarSignal.Blink=0              ; >0: toggle visibility every N frames
RadarSignal.AlwaysVisible=no     ; yes = also shown in shroud / when radar-invisible
RadarSignal.HideBlip=yes         ; hide the engine's coloured blip under the icon
```

- One image for both views: `Image` is drawn on the minimap at its native size
  and enlarged on the full-screen tactical map (`Image.Width * dst.Width /
  src.Width`), so the marker keeps the same proportion everywhere.
- On the minimap the marker is centred on the object's radar pixel and clipped
  to the radar rectangle; markers that would spill over the frame are skipped.
- While the tactical map is open the minimap copy is left untouched and the map
  draws the scaled image itself, so the marker is never painted twice.
- Scaling uses a block-mode (majority colour) filter, so pixel art does not blur;
  scaled frames are cached per `(image, frame, size)`.
- Multi-frame SHPs can be animated with `FrameCount` / `AnimRate`. A blinking
  marker can use an SHP with empty frames or `Blink`.
- `AlwaysVisible=no` keeps the vanilla rules: hidden in shroud unless the owner
  is the current player, and `RadarInvisible` / `RadarInvisibleToHouse` apply.
- The minimap marker is painted on the screen-sized surface right after the
  sidebar has been composited into it, so it stays put regardless of the
  sidebar's dirty rectangle bookkeeping.
- Objects sharing one radar pixel are each drawn; the last one drawn wins.
- Localisation: the feature itself has no on-screen text; all keys and log
  messages are English.

### Credits / 鸣谢

Ported from **[MagicShiba/MO_ExperienceEnhancements](https://github.com/MagicShiba/MO_ExperienceEnhancements)**
（MO_ExperienceEnhancements · 心灵终结体验优化拓展）.
All credit for the original design, reverse-engineering and implementations
goes to **MagicShiba** and the upstream contributors. Please support and check
out the original project.

---

---

## 13. Fog of War (TS-style)

A Tiberian Sun-style fog of war layered on the engine's fog:

- Explored cells never fall back to the black shroud; they show fog instead.
- Units inside fog are not drawn; buildings keep their last visible state
  (frozen snapshot, with the owner's palette and damage state).
- Terrain objects, overlays (walls, tiberium) and smudges are hidden while the
  cell is fully occluded.
- An "Reveal all" trigger clears both shroud and fog; by default fog stays off
  for the rest of the scenario.
- Vanilla gap generators are untouched and still create black shroud.

### Activation

The scenario's `[SpecialFlags]FogOfWar` decides per map. Campaign maps keep
their own value; skirmish and multiplayer may enable the game-mode option.
`TSFog=no` disables the whole port.

```ini
[General]
TSFog=yes             ; master switch
TSFog.ForceAll=no     ; yes = enable fog even for maps with FogOfWar=no (testing)
TSFog.RevealAll=clear ; clear = Reveal All also stops fog for the scenario
                      ; keep  = only clear the current fog
TSFog.FogFrame=34     ; fog frame used by FogProducer when AutoSpread=no
                      ; (AutoSpread=yes keeps the engine's per-cell frames)
```

### Fog spread

```ini
[General]
TSFog.AutoSpread=yes  ; yes = normal automatic fog of war
                      ; no  = explored cells stay fully visible after leaving
                      ;       sight (units stay live); only FogProducer spreads fog
```

### FogProducer

A building can act as a fog source that spreads fog outward from itself. It is
only active while fog of war is enabled. Every explored cell in its area that is
outside real friendly sight is fogged; cells the player can currently see stay
clear, and only objects in genuinely unseen cells are hidden.

A non-global producer only adds fog to its radius in `TSFog.AutoSpread=no`.
`FogProducer.Global=yes` forces the engine's normal fog over the whole map every
frame; own/allied real sight stays clear, unexplored cells stay black, and only
enemies outside real sight are hidden.

```ini
[BUILDINGTYPE]
FogProducer=no        ; master switch for this building
FogProducer.Global=no ; yes = normal fog over the whole map every frame;
                      ; real sight stays clear, unexplored stays black
FogProducer.Range=10  ; maximum radius in cells when Global=no
FogProducer.Speed=30  ; frames per spread step
FogProducer.Step=1    ; radius added per step
FogProducer.Grow=yes  ; yes = radius grows from 0 outward
                      ; no  = the whole radius is fogged immediately
```

Power loss pauses the growth; destroying the building clears its radius.
`Global=yes` forces the engine's normal fog over the whole map every frame. Own
and allied real sight stays clear, unexplored cells stay black, and only enemies
outside real sight are hidden. `Range`, `Grow` and power are ignored.

### Impact reveal

```ini
[General]
TSFog.ImpactReveal=yes  ; a detonation briefly reveals the impact area
TSFog.ImpactRadius=2    ; cells
TSFog.ImpactFrames=45   ; duration in frames
TSFog.AllowAnim=no      ; draw warhead animations inside explored fog
```

### Animation visibility

`TSFog.AllowAnim=yes` keeps combat animations drawing inside explored fog:
warhead `AnimList` / `SplashList` / `Crit.AnimList`, weapon `Anim` /
`OccupantAnim` / `AssaultAnim` / `OpenToppedAnim`, and bullet `Trailer`, together
with all animations linked from them (`TrailerAnim`, `ExpireAnim`, `BounceAnim`,
`Spawns`, `Next`, Phobos splash/wake/fire animations). Unexplored black shroud
still hides the animation.

Additional animation names can be registered in `[FogAnim]`; every name listed
there is allowed even if it is not reached through a combat field. Both the
comma-list and numeric-key forms are accepted (the section may be in rules or
art INI):

```ini
[FogAnim]
Anims=DUSTFOGA,LuzDisparomed
; or:
; 0=DUSTFOGA
; 1=LuzDisparomed
```

## Full INI key index

### `rulesmd.ini` → `[General]`
`AIDirectionalArmor`, `AIDirectionalArmor.Chance`, `AIDirectionalArmor.Delay`,
`AttachmentTopLayerMinHeight`, `AttachmentUndergroundLayerMaxHeight`,
`BattlePoints`, `BattlePoints.DefaultValue`, `BattlePoints.DefaultFriendlyValue`,
`CanBeBuiltOnOverlay.Remove`,
`DisableLoadGame.Difficulty`, `DisableSaveGame.Difficulty`,
`ExtendedAircraftMissions`, `LockGameSpeed.Difficulty`,
`MissileSpawnAttackCell`, `MovePositionIndicator.ShowEnemy`,
`RemoveShroudGlobally`, `ReverseMove`, `Rock.CanBeBuiltOn`, `SmudgeCapacity`,
`SmoothMove`, `TA.VirtualUnit`, `TacticalGear`, `TacticalPause`,
`TSFog`, `TSFog.AllowAnim`, `TSFog.AutoSpread`, `TSFog.ForceAll`, `TSFog.FogFrame`,
`TSFog.ImpactFrames`, `TSFog.ImpactRadius`, `TSFog.ImpactReveal`,
`TSFog.RevealAll`,
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
`Weapon<n>.Ammo*`, `Attachment<n>.*`, `AdvancedDrive*`, `AIDirectionalArmor*`,
`Armor.*Multiplier`, `AttackMove*`, `BattlePoints`, `Image.Empty`,
`PassengersPipOffset`, `PassengersPipSize`, `PenetratesTransport*`, `PipScale`,
`PipScale2`, `RadarSignal*`, `ShowMovePositionIndicator`, `SmoothMove`, `UrbanCombat`,
`WaterImage.Empty`, `FogProducer`, `FogProducer.Global`, `FogProducer.Grow`,
`FogProducer.Range`, `FogProducer.Speed`, `FogProducer.Step`

### `rulesmd.ini` → other sections
`[LoadBar]` (`Enable`, `MaxPerFrame`, `*.Default`),
`[LoadBarTypes]` (`Source`, `SourceIndex`, `Fill`, anchors, colours
including `Color.Gradient*`, text, visibility), `[LoadBarAssignments]`,
`[LoadBarAttachEffects]`, `Passengers.EnterDirections` (VehicleType),
`[AttachmentTypes]`, `[ArtilleryTypes]`, `[BomberTypes]`,
`[HouseType] BattlePoints` / `BattlePoints.CanUseStandardPoints`,
`[SuperWeaponType] BattlePoints.Amount` / `Charge*` / `LimboDelivery.Delay` /
`SpyPlane.Type` / `ParaDrop.Aircraft` / `SpawnPoints*`,
`[OverlayType] CanBeBuiltOn` / `CanBeBuiltOn.Remove` / `CustomPalette` /
`IgnoreObject`, `[SmudgeType] Unclearable` / `CustomPalette` / `DrawObject`,
`[AnimType] CreateOverlay*` / `CreateSmudge*`, `[WeaponType] Next` / `ExtraFire*`,
`[FogAnim] Anims` / numeric animation-name keys

### `RA2MD.INI` → `[Phobos]`
`ContinueGameOnExit`, `Formation*`, `MessageApplyHoverState`,
`MessageDisplayInCenter*`, `MOEE.*` (ported from MO_ExperienceEnhancements),
`RightClickCommands*`, `ShowGameTime*`, `TacticalZoom*`, `TextScale*`

### `uimd.ini`
`[TacticalZoom] *`, `[ToolTips] BattlePoints.Label`,
`[Sidebar] BattlePointsSidebar*`
