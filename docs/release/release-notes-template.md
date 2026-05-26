# Release notes template

> Use this template for every annotated release tag (v0.10.6 onward).
> Sections marked REQUIRED must always be present. OPTIONAL sections may be
> omitted when empty.

---

## vX.Y.Z-slug — YYYY-MM-DD

### What ships in this release

> REQUIRED. List only capabilities fully supported in this tag.
> Do NOT list partial, experimental, or deferred items here.

- [Feature / fix description — one sentence each.]

**Supported targets:** Linux x86_64 (host), Raspberry Pi Pico (RP2040),
ESP32-S3, STM32F4. See [hardware-validation.md](hardware-validation.md).

**ABI version:** X/Y/Z (see [docs/api-stability.md](../api-stability.md)).

**Wire format version:** vA.B / 0xNN (see
[docs/internals/bytecode-format.md](../internals/bytecode-format.md)).

### Bug fixes (OPTIONAL)

- [Bug fix description + issue / audit ID if applicable.]

### Breaking changes (OPTIONAL)

> If this release bumps ABI per the post-freeze policy, list each break here.
> Reference docs/api-stability.md §3.

None.

### Known deferrals (not in this release)

> REQUIRED. List items that are out of scope for this release but may be
> misunderstood as regressions or missing features.  Point at the tracking
> location (backlog, design-risks register, or roadmap).

- **[Item name]:** [One-sentence status.] Tracked in
  `docs/urbi-embedded-backlog.md` / `docs/urbi-embedded-design-risks.md` /
  `ROADMAP.md`.

### Test evidence

> REQUIRED (brief). Must reference releasetest output.

- `make releasetest` — N/N gates green.
- [Any sanitizer / cross-compile notes.]

### Upgrade guide (OPTIONAL)

> Include only if API changes require embedder-side changes.

None required from vX.Y.Z-previous.

---

## Notes for release authors

1. **"What ships"** is the positive claim. Be conservative. If a feature has
   known gaps, list it in "Known deferrals" instead.
2. **"Known deferrals"** prevents confusion when users discover missing items.
   Every entry should point at a tracking artifact so the user knows the gap
   is acknowledged.
3. **ABI + wire format versions** must be stated explicitly. Embedders depend
   on these to decide when to relink.
4. **Breaking changes** must reference the post-freeze policy. If the freeze
   was overridden, explain why.
5. Do not abbreviate the test evidence section — it is the primary audit trail
   for release quality.
