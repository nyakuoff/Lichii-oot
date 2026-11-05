# Ocarina of Time visual workaround (temporary)

This is a temporary, opt-in visual workaround for The Legend of Zelda: Ocarina of Time
(`0400486024DEC000`). It is not an emulator fix. It edits two shaders of this game so the
outdoor haze and exposure look closer to the reference. The root cause of the haze is still open.

Set `EDEN_NX2_OOT_VISUAL_WORKAROUND=1` to enable it. Any other value, or no value, leaves it off.
Each edited shader logs `TEMPORARY OoT visual workaround: shader <hash>, <n> scalar edits` at
warning level. An edit count of 0 means the shader no longer has the expected shape and was left
unchanged.

For experiments, set `EDEN_NX2_OOT_FOG_SCALE` (default `0.25`) and/or
`EDEN_NX2_OOT_EXPOSURE_SCALE` (default `0.65`). These accept complete finite nonnegative
numbers only (no whitespace or trailing text); invalid values warn and use the default.
The selected scales are logged once when the Vulkan pipeline cache is created with the
workaround enabled. Restart the emulator after changing these variables.

Existing disk caches do not need clearing: the emulator caches shader environments, not
the edited IR, and both disk-load and runtime compilation pass through the same IR edit.
The Vulkan driver cache receives the newly emitted SPIR-V, so it cannot bypass the edit
or reuse a different scale's shader. No cache format or shader hash ABI is changed.
These shader hashes are the lowered-code CityHash64 XOR `0x4f554e4345000002`, including
the converted header and native address map, as used by frame dumps, pipeline keys and
`reject_shaders` / `EDEN_NX2_REJECT_HASH`; they are not the native capture keys below.

## What it changes

| Shader | Native key | Edit |
|---|---|---|
| Fog integration, compute `780c0536b89fc89e` | `ec68e40eb783ff32` | Multiplies the integration slice distance by 0.25 before the extinction and scattering accumulation. |
| Log-bloom final, fragment `bded5ce917183214` | `e82daf057f4b81d6` | Multiplies the three loads of the exposure constant (cbuf 3, offset 0x38) by 0.65 before the filmic curve. |

The adaptive final (fragment `6cb06edf14be1d79`, native `1d3239dd29ff4f88`) and the simple-bloom
final are not edited. No captured program matches the simple-bloom final.

## Limitations

- Interiors use the same fog integration shader, so the shop and other interiors also get the
  reduced fog while the workaround is on.
- Interiors drawn with the adaptive final do not get the exposure change.
- The workaround needs an outdoor and indoor A/B with the workaround on and off before it can be
  called validated.
