<script lang="ts">
  import type { Snippet } from "svelte";
  import type { AbilityBreakdown, Breakdown, BuildBreakdown, Source } from "./engine";

  let {
    breakdown,
    build,
    ability,
    defense,
    distance,
    hitOverrides,
    onhits,
    chart,
  }: {
    breakdown: Breakdown;
    build: BuildBreakdown;
    ability?: AbilityBreakdown;
    defense: number;
    distance: number;
    hitOverrides: (number | null)[];
    /** Sets a subattack's hits per volley by hand, or clears it with null. */
    onhits: (index: number, hits: number | null) => void;
    chart?: Snippet;
  } = $props();

  const whole = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });
  const fine = new Intl.NumberFormat("en", { maximumFractionDigits: 2 });

  // Plain-language names for the client tags the engine can't model yet.
  const mechanicNames: Record<string, string> = {
    OnPlayerShootActivate: "a proc when shooting",
    OnEnemyHitActivate: "a proc on hit",
    OnPlayerAbilityActivate: "a proc on ability use",
    TurnRate: "homing or turning shots",
    TurnRateDelay: "homing or turning shots",
    TurnStopTime: "homing or turning shots",
    TurnAcceleration: "homing or turning shots",
    TurnAccelerationDelay: "homing or turning shots",
    TurnClamp: "homing or turning shots",
    CircleTurnAngle: "circling shots",
    CircleTurnDelay: "circling shots",
    CollisionMult: "a larger hitbox",
    ProjectilePattern: "alternating shot patterns",
    DefaultAngleIncr: "shifting shot angles",
    Meter: "a charge meter",
    DamageMultiplier: "Vulnerable (more damage taken)",
    MpCostPerSecond: "an MP cost per second",
    MpEndCost: "an MP cost when it ends",
    MultiPhase: "several phases",
    StartUse: "a hold to use",
    EndUse: "a hold to use",
    OnDetonateHexActivate: "hex effects",
    OnConditionEndActivate: "effects when a status ends",
    OnPlayerHitActivate: "a proc when hit",
    OnPlayerHealActivate: "a proc when healed",
    BulletCreate: "shots created around the cursor",
    SpawnCreep: "summons",
    RaiseDead: "summons",
    DetonateHex: "hex detonation",
    ShurikenAbility: "shuriken charges",
    VampireBlast: "life-draining blasts",
    DamageMultAura: "damage taken or dealt multipliers",
    ObjectToss: "thrown objects",
    ChannelDash: "dashes",
    GenericActivate: "a scripted effect",
    EffectBlast: "a blast effect",
  };

  const describe = (tags: string[]) => [
    ...new Set(
      tags.map((tag) => (tag.startsWith("ActivateOnEquip ") ? "an on-equip effect" : (mechanicNames[tag] ?? tag))),
    ),
  ];
  const unsupported = $derived(describe(breakdown.unsupported));
  const abilityUnsupported = $derived(ability ? describe(ability.unsupported) : []);
  const percent = new Intl.NumberFormat("en", { style: "percent", maximumFractionDigits: 0 });

  const allAssumptions = $derived([
    ...new Set([...breakdown.assumptions, ...(ability?.assumptions ?? []), ...build.assumptions]),
  ]);

  function buffName(b: { effect: string; amount: number }): string {
    return b.amount ? `+${b.amount} ${b.effect}` : b.effect;
  }

  const sourceLabels: Record<Source, string> = {
    wiki: "Wiki",
    "client-data": "Game data",
    "geometric-estimate": "Estimate",
    curated: "Curated",
    user: "You",
    assumed: "Assumed",
  };

  function setHits(index: number, input: HTMLInputElement) {
    onhits(index, input.value === "" ? null : Math.max(0, Number(input.value)));
  }

  const wikiPages: Record<string, string> = {
    "RealmEye: Character Stats": "https://www.realmeye.com/wiki/character-stats",
    "RealmEye: Status Effects": "https://www.realmeye.com/wiki/status-effects",
    "RealmEye: Weapon Attributes": "https://www.realmeye.com/wiki/weapon-attributes",
  };
</script>

<section class="results" aria-live="polite">
  <div class="hero">
    <span class="value">{whole.format(build.dps)}</span>
    <span class="label">
      DPS against {defense} DEF, {distance} tiles away{#if ability && ability.dps > 0}: {whole.format(build.weaponDps)}
        from the weapon and {whole.format(ability.dps)} from the ability{/if}
    </span>
  </div>

  {#if unsupported.length}
    <p class="notice">
      <strong>Not modelled yet:</strong>
      {unsupported.join(", ")}. The real damage can differ from what's shown here.
    </p>
  {/if}

  {@render chart?.()}

  {#if ability}
    <h2>Ability</h2>
    <dl class="figures">
      <div><dt>Casts per second</dt><dd>{fine.format(ability.castsPerSecond)}</dd></div>
      <div><dt>MP cost</dt><dd>{whole.format(ability.mpCost)}</dd></div>
      <div><dt>MP per second</dt><dd>{fine.format(ability.mpPerSecond)}</dd></div>
      <div><dt>Damage per cast</dt><dd>{whole.format(ability.damagePerCast)}</dd></div>
    </dl>
    {#if ability.parts.length}
      <ul class="plain">
        {#each ability.parts as p (p.name)}
          <li>{p.name}: {whole.format(p.damage)} damage per cast</li>
        {/each}
      </ul>
    {/if}
    {#if build.uptimes.length}
      <p class="buffs">
        Weapon DPS goes from {whole.format(breakdown.dps)} to {whole.format(build.weaponDps)} with
        {#each build.uptimes as u, i (i)}{i > 0 ? (i === build.uptimes.length - 1 ? " and " : ", ") : ""}{buffName(u)}{u.on ===
          "target"
            ? " on the target"
            : ""} up {percent.format(u.share)} of the time{/each}.
      </p>
    {/if}
    {#if abilityUnsupported.length}
      <p class="notice">
        <strong>Ability effects not modelled yet:</strong>
        {abilityUnsupported.join(", ")}. The real damage can differ from what's shown here.
      </p>
    {/if}
  {/if}

  <h2>{ability ? "Weapon" : "How it adds up"}</h2>
  <div class="table-scroll">
    <table>
      <thead>
        <tr>
          <th scope="col">Subattack</th>
          <th scope="col">Volleys/s</th>
          <th scope="col">Hits/volley</th>
          <th scope="col">Damage/hit</th>
          <th scope="col">DPS</th>
        </tr>
      </thead>
      <tbody>
        {#each breakdown.subattacks as s, i (i)}
          <tr>
            <td>{i + 1}</td>
            <td>{fine.format(s.volleysPerSecond)}</td>
            <td class="hits">
              <input
                type="number"
                min="0"
                max={s.projectilesPerVolley}
                step="0.5"
                aria-label="Hits per volley for subattack {i + 1}, out of {s.projectilesPerVolley}"
                placeholder={fine.format(s.hitsPerVolley)}
                value={hitOverrides[i] ?? ""}
                onchange={(e) => setHits(i, e.currentTarget)}
              />
              <span class="of">of {s.projectilesPerVolley}</span>
              <span class="hit-source">{sourceLabels[s.hitsSource]}</span>
            </td>
            <td>{fine.format(s.damagePerHit)}</td>
            <td>{whole.format(s.dps)}</td>
          </tr>
        {/each}
      </tbody>
    </table>
  </div>

  <p class="hint">
    Type in the hits column to set hits per volley by hand; clear it to go back to the estimate.
  </p>

  <h2>Where the numbers come from</h2>
  <ul class="terms">
    {#each [...breakdown.terms, ...(ability?.terms ?? [])] as t (t.name)}
      <li>
        <span class="term-name">{t.name}</span>
        <span class="term-value">{fine.format(t.value)}</span>
        <span class="source {t.source}">
          {#if wikiPages[t.reference]}
            <a href={wikiPages[t.reference]} target="_blank" rel="noreferrer">{sourceLabels[t.source]}</a>
          {:else}
            {sourceLabels[t.source]}
          {/if}
        </span>
      </li>
    {/each}
  </ul>
  {#if allAssumptions.length}
    <p class="assumptions">
      <strong>Assumed, with no source yet:</strong>
      {allAssumptions.join("; ")}.
    </p>
  {/if}
</section>

<style>
  .results {
    display: grid;
    grid-template-columns: minmax(0, 1fr);
    gap: 1rem;
    align-content: start;
    min-width: 0;
  }
  .hero {
    display: grid;
  }
  .hero .value {
    font-size: 3.5rem;
    font-weight: 600;
    line-height: 1.1;
  }
  .hero .label {
    color: var(--text-2);
  }
  .notice {
    margin: 0;
    padding: 0.6rem 0.8rem;
    border-radius: 8px;
    background: var(--warn-bg);
    color: var(--warn-text);
  }
  h2 {
    margin: 0.5rem 0 0;
    font-size: 1rem;
  }
  .table-scroll {
    overflow-x: auto;
  }
  table {
    border-collapse: collapse;
    width: 100%;
    font-variant-numeric: tabular-nums;
    font-size: 0.9rem;
  }
  th,
  td {
    padding: 0.35rem 0.5rem;
    border-bottom: 1px solid var(--grid);
    text-align: right;
  }
  th:first-child,
  td:first-child {
    text-align: left;
  }
  th {
    font-weight: 600;
    color: var(--text-2);
    white-space: nowrap;
  }
  .terms {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    gap: 0.25rem;
    font-size: 0.9rem;
  }
  .terms li {
    display: grid;
    grid-template-columns: 1fr auto 6.5rem;
    gap: 0.75rem;
    padding: 0.2rem 0;
    border-bottom: 1px solid var(--grid);
  }
  .term-value {
    font-variant-numeric: tabular-nums;
  }
  .source {
    color: var(--text-2);
    text-align: right;
  }
  .source a {
    color: var(--accent);
  }
  .hits {
    white-space: nowrap;
  }
  .hits input {
    width: 3.5rem;
    font: inherit;
    text-align: right;
    color: var(--text);
    background: var(--page);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 0.1rem 0.3rem;
  }
  .hits input::placeholder {
    color: var(--text);
  }
  .of {
    color: var(--text-2);
    margin-left: 0.25rem;
  }
  .hit-source {
    display: block;
    font-size: 0.75rem;
    color: var(--muted);
  }
  .hint {
    margin: 0;
    font-size: 0.85rem;
    color: var(--text-2);
  }
  .figures {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(8rem, 1fr));
    gap: 0.5rem 1rem;
    margin: 0;
  }
  .figures dt {
    font-size: 0.8rem;
    color: var(--text-2);
  }
  .figures dd {
    margin: 0;
    font-weight: 600;
    font-variant-numeric: tabular-nums;
  }
  .plain {
    margin: 0;
    padding-left: 1.2rem;
    font-size: 0.9rem;
  }
  .buffs {
    margin: 0;
    font-size: 0.9rem;
  }
  .assumptions {
    margin: 0;
    font-size: 0.9rem;
    color: var(--text-2);
  }
</style>
