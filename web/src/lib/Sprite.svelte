<script lang="ts">
  // One sprite out of sprites.png, scaled up with crisp pixels and centred
  // in a square `size` pixels wide.
  import type { Rect, Sprites } from "./sprites";

  let {
    sprites,
    rect,
    size = 40,
    label = "",
  }: {
    sprites: Sprites | null;
    rect: Rect | undefined;
    size?: number;
    /** Read out instead of the picture; empty for a decorative sprite. */
    label?: string;
  } = $props();

  const scale = $derived(rect ? size / Math.max(rect[2], rect[3]) : 1);
</script>

<span class="box" style:width="{size}px" style:height="{size}px">
  {#if sprites && rect}
    <span
      class="sprite"
      role={label ? "img" : undefined}
      aria-label={label || undefined}
      aria-hidden={label ? undefined : "true"}
      style:width="{rect[2] * scale}px"
      style:height="{rect[3] * scale}px"
      style:background-image="url({sprites.image.src})"
      style:background-size="{sprites.image.naturalWidth * scale}px {sprites.image.naturalHeight * scale}px"
      style:background-position="-{rect[0] * scale}px -{rect[1] * scale}px"
    ></span>
  {/if}
</span>

<style>
  .box {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    flex: none;
  }
  .sprite {
    display: block;
    background-repeat: no-repeat;
    image-rendering: pixelated;
  }
</style>
