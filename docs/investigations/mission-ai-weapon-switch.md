# Mission wingmen remain in weapon selection

Live decision logging showed visible, living, attackable mission enemies, with
damage protection inactive. Some wingmen repeatedly requested ranged slot 1
while the actual bound slot remained 0 or 2. Their shot counters stopped advancing
and the old pending-switch branch continued lateral movement without attacking.

The pinned native `CNPC::ChangeWeapon` at `0x0065EBB0` compares the requested slot
to `unit+0xDD0`, then skips an equal request. That hint is distinct from the actual
`AttackFSM+0x240` selection and `+0x248` weapon binding. The hint itself was not
sampled in that live session; offline fixtures reproduce the stale-hint failure.
Lua slots are already zero-based: the native FIRST/SECOND bindings are 0/1.

The owned-AI adapter now submits the same native internal `0x48` message with its
slot byte at `+0x10`, through the same virtual receiver. It does not write the
selected slot, grant weapons, alter cooldowns or bypass the receiver's permission
checks. Selection acknowledgement checks the actual slot and weapon identity;
special selection also requires the actual special attack style.

Medium/Hard keep legal output from the currently bound weapon while requesting a
better one. If it cannot reach the enemy, they approach instead of circling in a
selection loop. A forced ranged response to melee pressure still forbids a sword
counter. Special preparation retains legal current-gun output; any valid ordinary
weapon after a special returns control immediately, without waiting for a preferred
previous slot. Native hurt, ammo, cooldown and action restrictions still apply.

Regression evidence includes Lua controller cases, a real core/DLL/Lua fixture
with a stale hint, deferred native acknowledgement and an out-of-context request,
plus exact native instruction contracts. Real mission combat after installation
remains a separate manual acceptance step.
