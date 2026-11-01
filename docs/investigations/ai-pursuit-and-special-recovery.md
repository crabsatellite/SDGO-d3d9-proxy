# AI pursuit, target geometry and special recovery (0.4.2)

Live mission snapshots showed repeated native path failures followed by stop and
retry on every decision. The native route ABI was correct: the map region simply
had no usable start/end navigation nodes. The ordinary movement retry policy was
not respecting its deadline after the motor stopped. A second defect failed to
restart a stopped motor when its cached waypoint was unchanged.

The controller now retains legal fire, throttles failed route queries independently
of motor flags, and uses bounded native walking goals when full routes are absent.
The short step preserves height, respects action/movement owners and leaves physical
collision and the horizontal stuck watchdog active. Close-combat preference reads
the native mastered-weapon attribute from the optional read-only catalog provider,
with a weapon-data fallback. Near engagements take priority before comparable-distance
low-HP focus; Hard cover/strafe/orbwalk cannot silently replace a melee closing goal.

Target geometry now follows the supported native attack tests: sphere center for
kind 7, OBB center for kind 0x12, and node position for kind 0x15. Gun range uses the
native muzzle; melee uses the owner's sphere center. The target's navigation root
can lie outside the map while its combat center remains hittable. Actual weapon
range is never clipped by the extra-perception cap. Per-weapon visibility preserves
native ranged-versus-melee friendly collision rules, ignores owner attachment scenes,
and continues to respect walls/terrain. Valid opaque ordinary weapons remain usable;
the special slot and style-2 records cannot compete as ordinary weapons.

Special recovery required a separate native correction. The ordinary attack-off
message also queues AttackFSM Neutral, which can cut an active action. It is not
a safe key-release API. The new owned-AI input-only bridge clears held input latches
without changing current/pending FSM pointers. Native busy/neutral and actual weapon
binding determine handoff after normal completion, misses and missing finish notices.
A stale special receipt cannot clear a later ordinary melee combo. Tests include a
counterexample with the real attack-off transition semantics, not just a boolean mock.

Independent hostile reviews covered native ABI/collision, pursuit/motor ownership,
and melee/ordinary/special policy. Their findings produced regressions for stopped
same-waypoint recovery, failed-path stop storms, separate sight/range origins, long
legal ranges, hidden-target memory, opaque weapons and special lifecycle recovery.
The real core/DLL/Lua fixtures and all automated AI checks pass. These remain isolated
and static/native-contract evidence; real mission play after installation is a
separate manual acceptance step. Non-unit mission structures held only in the raw
object registry remain outside the combat-unit enumerator's supported scope.
