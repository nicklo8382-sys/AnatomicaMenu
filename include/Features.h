#pragma once

// Build switches.
//
// ANATOMICA_CUM: Anatomica's own cum (drips and strands from the hole, squirts, sprays, oozes and gushes, the string on
// pull-out, trails down the leg, puddles, cum held inside, cum film / froth on the rim, the giver's coated schlong) and
// its settings page. Off for now: cum is to come from an OStrands (or similar) integration instead. The code stays in
// the source so it can be switched back on; with this at 0 none of it runs and none of its settings are shown.
// Wetness (the gloss on the hole, the clear wet layer, water beads, the giver's wet shine) is not part of this switch.
#ifndef ANATOMICA_CUM
#define ANATOMICA_CUM 0
#endif

inline constexpr bool kCum = ANATOMICA_CUM != 0;
