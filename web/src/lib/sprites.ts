// Sprites cut from the game client by the data extractor and published next
// to the dataset. They're optional: without them the simulation view draws
// plain shapes. The format is described in the README.

/** x, y, width, height in pixels in sprites.png. */
export type Rect = [number, number, number, number];

export interface SpriteIndex {
  schemaVersion: number;
  build: string;
  projectiles: Record<string, { rect: Rect; angleCorrection?: number; rotation?: number }>;
  weapons: Record<string, Rect>;
  classes: Record<string, { stand: Rect; attack: Rect[] }>;
  target: Rect;
}

export interface Sprites {
  index: SpriteIndex;
  image: HTMLImageElement;
}

const supportedVersion = 1;

/** Null when there are no sprites, or they're a version this site can't read. */
export async function loadSprites(): Promise<Sprites | null> {
  try {
    const response = await fetch(`${import.meta.env.BASE_URL}data/sprites.json`);
    if (!response.ok) {
      return null;
    }
    const index = (await response.json()) as SpriteIndex;
    if (index.schemaVersion !== supportedVersion) {
      return null;
    }
    const image = new Image();
    image.src = `${import.meta.env.BASE_URL}data/sprites.png`;
    await image.decode();
    return { index, image };
  } catch {
    return null;
  }
}
