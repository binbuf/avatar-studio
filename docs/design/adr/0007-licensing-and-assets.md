# ADR-0007: MIT application, original assets only, per-asset provenance

- **Status:** Proposed
- **Date:** 2026-11-20
- **Related:** [`../product-overview.md`](../product-overview.md#legal-posture),
  [`../security-privacy.md`](../security-privacy.md#dependency-and-asset-manifest)

## Context

The project reimplements a well-known creator's visual effect. Three separable concerns are often
conflated:

1. **The technique** — projecting live facial regions onto a rigid mesh, head-tracked. Methods and
   ideas are generally not protected by copyright.
2. **The character** — a specific peanut model, its textures, its artwork. Clearly protected.
3. **The brand** — the creator's name, logo, and trade dress, including a character silhouette that
   functions as a mark. Trademark, not copyright, and a different analysis.

The source brief asserted that specific trademark applications exist. That claim was not verified and
must not be load-bearing. Nothing in this ADR is legal advice; it is an engineering policy that
keeps the project in a defensible position.

## Decision

1. **The application is MIT licensed**, with a complete `NOTICE` and a per-dependency manifest.
2. **All bundled characters, props and textures are original or CC0**, with recorded provenance
   (author, source, license, hash). No asset enters `examples/` without it.
3. **No resemblance to a specific creator's character or branding.** No peanut-shaped default, no
   similar name, no "inspired by" marketing. The bundled examples are deliberately unrelated shapes.
4. **Users may import anything they have rights to.** The app does not police imports, does not
   distribute them, and does not host a library in 1.0.
5. **No GPL/AGPL in the shipped binary.** A GPL component may be used in a separate process only with
   legal sign-off recorded in an ADR.
6. **Licence audit is a build gate**, not a checklist item: CI fails on manifest drift or a denied
   licence.

## Alternatives considered

| Option | Why not |
|---|---|
| **GPL-3.0 for the app** | Would match OBS's licence and simplify any libobs-based plugin, but forbids the commercial and closed integration uses that "permissively licensed" implies. Contradicts the product goal. |
| **Apache-2.0** | Equally good; MIT is simpler and matches godot-cpp. Either is acceptable; MIT chosen for consistency and brevity. |
| **Ship a "peanut-like" default character** | Maximum recognisability, maximum risk, and it makes the product look like a clone instead of a tool. Rejected on both grounds. |
| **Source-available / non-commercial** | Excludes exactly the users the project is meant to serve, and is incompatible with the FOSS framing. |
| **No bundled assets at all** | Removes the onboarding path; users would need a model before seeing anything work. Bad product. |
| **Rely on the source brief's legal summary** | Unverified claims about trademark filings, and no clearance opinion. Not a basis for a decision. |

## Consequences

**Positive**

- Clear, defensible position: a tool, not a clone.
- Users can build commercial products with it.
- Provenance metadata makes takedown requests tractable and asset replacement cheap.

**Negative**

- Default characters are less striking than a well-known one would be; onboarding relies on quality
  rather than recognition.
- Provenance discipline slows asset contribution and must be enforced socially as well as in CI.
- MIT means downstream forks may not share improvements back.

**Neutral**

- The licence choice does not constrain the architecture; it constrains dependencies, which is
  already a design constraint ([`../architecture.md`](../architecture.md#build-and-distribution)).

## Follow-ups

1. Legal review of the policy and the bundled asset licences before M5. This ADR does not substitute
   for it.
2. Verify the licence of the MediaPipe model weights in writing, separately from the source licence.
3. Publish `SECURITY.md` and a contribution guide that states the provenance requirement.
4. Keep a recorded decision trail: if a future feature requires a GPL dependency, it gets its own
   ADR, not a quiet exception.
