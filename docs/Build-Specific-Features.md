# Build-Specific Features

This page documents the systems added or modified in this build on top of the
upstream Phobos feature set. The root `GC-New-Features.md` contains the full
change list and examples; this page is the reference for the INI keys that are
not covered by the upstream documentation.

## `[General]` master switches

```ini
[General]
AresWeaponAmmo=yes        ; boolean - per-weapon ammo pools / ExtraFire ammo
MOEE.Disabled=no          ; boolean - MO_ExperienceEnhancements gameplay + commands
```

- `AresWeaponAmmo=no` disables every `Ammo.*` weapon pool and falls back to the
  vanilla unit `Ammo`; ExtraFire entries become unlimited. See
  [Per-weapon ammo pools](New-or-Enhanced-Logics.md#per-weapon-ammo-pools).
- `MOEE.Disabled=yes` turns off the MOEE gameplay modules and their commands.
  Diagnostics and crash logging stay active.

## MO_ExperienceEnhancements port

The `src/MOEE/` modules are configured with `[General] MOEE.*` keys in
`rulesmd.ini`. The full feature descriptions and defaults are in
`GC-New-Features.md` (section 12). Keys:

```ini
[General]
MOEE.Disabled=no
MOEE.MegaListCap=yes
MOEE.Observer=yes
MOEE.WaypointCap=128
MOEE.OrderThrottle=1
MOEE.RepairAll=0
MOEE.RepairAll.Interval=30
MOEE.RepairAll.Batch=24
MOEE.RepairAll.DoubleClick=yes
MOEE.RepairAll.DblFrames=12
MOEE.PlaceScatter=1
MOEE.PlaceScatter.Interval=30
MOEE.PlaceScatter.Batch=16
MOEE.PlaceScatter.Dist=2
MOEE.TreeChop.Range=4
MOEE.FuzzyAttack.Range=4
MOEE.FuzzyAttack.SquadSize=4
MOEE.SmartOrder.NodeStep=2
MOEE.PatrolCircle.Size=4
MOEE.PatrolCircle.Commit=yes
MOEE.SmartArea.Range=4
MOEE.SmartArea.SquadSize=4
MOEE.DeployPreview=yes
MOEE.DeployPreview.AutoDeploy=yes
MOEE.DeployPreview.Timeout=675
MOEE.MapCenter=yes
MOEE.WeaponRange=yes
MOEE.VecText=yes
MOEE.VecText.FontFace=HarmonyOS_Sans_SC_Regular.ttf
MOEE.VecText.FontFaceLatin=Roboto_SemiCondensed-Medium.ttf
MOEE.VecText.FontFaceBold=HarmonyOS_Sans_SC_Bold.ttf
MOEE.VecText.FontFaceLatinBold=Roboto_SemiCondensed-Bold.ttf
MOEE.VecText.FontPx=0
MOEE.VecText.Bold=no
MOEE.VecText.DigitTrack=0
MOEE.VecText.CjkOnly=yes
MOEE.VecText.MenuTake=yes
MOEE.VecText.CreditPx=0
MOEE.VecText.CreditDY=0
```

Commands: Attack Move, Fuzzy Attack, Patrol Circle, Repair All, Smart Area,
Smart Order, Tree Chop, Weapon Range and Deploy Preview. Bind them in
Options -> Keyboard.

## GiftBox

A dummy unit spawns one or more real units after a delay, then removes or
destroys itself. Works on infantry, vehicles and aircraft; buildings are
ignored. Config is on the dummy TechnoType.

```ini
[SOMEDUMMY]
GiftBox.Types=REALUNIT,REALUNIT2 ; list of TechnoType sections
GiftBox.Nums=1,2                 ; count per entry (single value applies to all)
GiftBox.RandomWeights=100,50     ; weights when RandomType=yes
GiftBox.Chances=1.0,0.5          ; 0.0-1.0 chance per entry
GiftBox.RandomType=no            ; yes = pick by weight instead of spawning all
GiftBox.UseChancesAndWeight=no   ; apply weight as a percent in the fixed list
GiftBox.Remove=yes               ; remove the dummy when it opens
GiftBox.Destroy=no               ; destroy via death path (alias GiftBox.Explodes)
GiftBox.Delay=0                  ; frames before opening
GiftBox.RandomDelay=0            ; extra random 0..N frames
GiftBox.RandomRange=0            ; scatter radius in cells (alias GiftBox.CellRandomRange)
GiftBox.EmptyCell=no             ; only unoccupied cells (alias GiftBox.RandomToEmptyCell)
GiftBox.InheritAmmo=no           ; copy the dummy's Ammo to the spawned units
```

The dummy is removed before the gifts unlimbo, which is required for infantry
boxes. `Destroy=yes` uses the normal death path; `Remove=yes` with
`Destroy=no` limboes the dummy.

## AbilityTypes

Named abilities attached to TechnoTypes through `Abilities=` or the independent
`Ability1`..`Ability12` slots. Active abilities are fired from the ability UI;
passive abilities trigger automatically.

```ini
[AbilityTypes]
0=SOMEABILITY

[SOMEABILITY]
UIName=NAME:SOMEABILITY
Description=NAME:SOMEABILITY_DESC
Icon=ABILITYICON.PCX
Activation=active|passive          ; default active
Target=self|ally|enemy|cell        ; default self
Range=0                            ; cells
AffectHouses=all|owner|allies|enemies|team|notallies|notowner
Cooldown=0                         ; frames
Charges=-1                         ; -1 = infinite
Recharge=0                         ; frames per charge restored
Passive.Trigger=always|interval|ondamaged  ; default always
Interval=0                         ; frames between passive triggers
Duration=0                         ; AttachEffect duration override (0 = AE default, -1 = permanent)
Weapon=                            ; optional WeaponType
Weapon.Instant=no
Weapon.ReleaseAtSelf=no
Weapon.FLH=x,y,z
Weapon.UseFirepowerMultiplier=yes
Weapon.UseUnitAmmo=no
Weapon.PlayFiringEffects=yes
AttachEffects=                     ; optional AttachEffectType list
Heal.Amount=0
Heal.Percent=0.0
Repair.Amount=0
Repair.Percent=0.0
Animation=
Sound=-1
```

```ini
[SOMETECHNO]
Abilities=SOMEABILITY,OTHER
; or independent slots:
Ability1=SOMEABILITY
Ability2=
Ability3=OTHER
```

`Ability1..Ability12` are independent slots: a missing key leaves an empty slot
and later keys are still read. `Abilities=` is the list form and takes
precedence. `Target=cell` requires `Weapon`; otherwise the ability logs a
warning and does not activate.

## Superweapon extensions

```ini
[SuperWeaponType]
SW.UseAITargeting=no     ; AI may choose a target
SW.AutoFire=no           ; fire automatically when charged
SW.ManualFire=yes        ; allow manual firing
SW.ShowCameo=yes         ; show the sidebar cameo
SW.Unstoppable=no        ; charge-drain cannot be interrupted
SW.MaxCount=-1           ; max simultaneous instances (-1 = unlimited)
SidebarPCX=              ; custom sidebar cameo image

Message.CannotFire=
Message.InsufficientFunds=
Message.Color=-1         ; color scheme index
Message.FirerColor=no    ; use the firer's house color

EVA.Impatient=-1
EVA.InsufficientFunds=-1
EVA.SelectTarget=-1
```

`SW.AutoFire=yes` with `SW.ShowCameo=no` creates a hidden automatic
superweapon. `SW.ManualFire=no` removes the manual button while keeping the
automatic behaviour. `SW.Unstoppable` only applies to charge-drain
superweapons.

## Sidebar BattlePoints

```ini
[GDI]                 ; Side section
Sidebar.BattlePoints.Offset=0,0   ; X,Y pixels, default 0,0
Sidebar.BattlePoints.Color=       ; R,G,B, default unset
Sidebar.BattlePoints.Align=left   ; left | center | right, default left
```

## Recall Carrier Aircraft / Stop hotkey

`Recall Carrier Aircraft` sends the selected carrier payload, or every airborne
payload of the selected carrier, back to its deck. It has no INI key; bind it in
Options -> Keyboard.

The build also wraps the vanilla **Stop** command: with an airborne carrier
payload selected, Stop queues the normal `Mission::Enter` recall
(state 2 -> 3 -> 4 deck landing). For every other selected object the original
Stop behaviour is forwarded unchanged. A parked payload ignores the order
because it is already home.

Carrier payload combat rules:

- A direct player attack click or Ctrl force attack on a parked payload is
  registered as an explicit order and may use ground weapons against it.
- AI and auto-acquired targets are never forced, so they cannot drain ammo on
  the deck every frame.
- Forced attacks still respect range, rate of fire and ammo, including the
  vanilla `NoAmmoWeapon` switch.
- Waypoint/queued attacks are not part of the click registration; use a direct
  attack or Ctrl attack when the forced-attack exception is required.
- `AircraftCarrier.DockRadius` limits player move orders around the deck slot;
  see [Aircraft carrier deck](New-or-Enhanced-Logics.md#aircraft-carrier-deck).

## RadarSignal

Attaches a custom SHP marker to any TechnoType (infantry, vehicle, aircraft or
building) on the minimap instead of the engine's one-pixel blip.

```ini
[GAPOWR]
RadarSignal=yes                  ; master switch
RadarSignal.Image=MYICON         ; SHP name; .shp is appended when missing
RadarSignal.Palette=myicon.pal   ; optional, defaults to sidebar.pal
RadarSignal.Frame=0              ; first frame
RadarSignal.FrameCount=1         ; frames to cycle, 1 = static
RadarSignal.AnimRate=6           ; logic frames per animation frame
RadarSignal.Blink=0              ; >0: toggle visibility every N frames
RadarSignal.AlwaysVisible=no     ; yes = also shown in shroud / when radar-invisible
RadarSignal.HideBlip=yes         ; hide the engine's coloured blip under the icon
```

- The same image is drawn on the minimap at native size and enlarged on the
  tactical map, so the marker keeps its proportion in both views.
- Multi-frame SHPs animate with `FrameCount` / `AnimRate`; `Blink` can toggle
  visibility. `AlwaysVisible=no` keeps the vanilla shroud and
  `RadarInvisible` rules.
- Markers are clipped to the radar rectangle; markers that would spill over are
  skipped. Objects sharing one radar pixel are all drawn.

## Additional keys

These keys are implemented in the build and documented in
`GC-New-Features.md`; they are listed here for completeness.

```ini
[General]
MissileSpawnAttackCell=no        ; missile spawner targets the attack cell
RemoveShroudGlobally=no          ; warhead shroud removal is global

[AudioVisual]
AirstrikeLineZAdjust=0           ; airstrike line Z offset
VoxelShadowLightSource=no        ; use the light source for voxel shadows
LaserThickness=1.0               ; default laser thickness

[TechnoType]
VoiceIFVRepair=                  ; IFV repair voice
TiberiumEater.DisplayOffset=0,0  ; Tiberium eater counter offset
TiberiumEater.DisplayToHouse=no  ; show the counter to the owner house

[SuperWeaponType]
EMPulse.Cannons=0                ; EMPulse cannon count

[WarheadType]
AffectsOwner=                    ; nullable, default follows AffectsEnemies
Crit.AffectAbovePercent=0.0      ; critical hit only above this health percent
InflictLocomotor=no              ; inflict the warhead's Locomotor on hit
RemoveInflictedLocomotor=no      ; remove the inflicted locomotor

[WeaponType]
ProjectileRange=0                ; projectile/tracer range override
Beam.Duration=0                  ; beam duration
Beam.IsHouseColor=no             ; beam uses the house color
Bolt.ParticleSystem=             ; bolt particle system

[ProjectileType]
Trajectory.Sample.TargetSnapDistance=0.0
```

## Diagnostics

```ini
[Phobos]                         ; RA2MD.INI
Debug.AircraftCarrier=false      ; verbose carrier deck messages
Debug.CarrierArtDiag=false       ; carrier art/deck calibration diagnostics
Debug.CarrierSelfTest=false      ; in-game carrier self-test
GetFireError.Guard=true          ; sanity guard around GetFireError
GetFireError.Trace=true          ; write GetFireError crash/context traces
```

`Debug.FootprintOverlay` (default `false`) is a separate visual aid for
multi-cell hulls and is independent of the MOEE master switch.
