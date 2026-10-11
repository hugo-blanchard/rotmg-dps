# rotmg-dps

A DPS calculator for Realm of the Mad God (Exalt). It works out how much damage a build does to a given target, using item data from the game client and formulas sourced from the RealmEye wiki. A small simulation shows the character firing at a dummy so the numbers are easy to check.

Work in progress. There's nothing to run yet.

## Notice

Unofficial fan tool, not affiliated with DECA. Game assets © DECA Live Operations GmbH.

## Building

The engine is C++20 and needs CMake 3.25 or newer. Tests use Catch2, which CMake downloads on the first configure.

```
cmake -S engine -B engine/build
cmake --build engine/build
ctest --test-dir engine/build
```

Pass `-DROTMG_SANITIZE=ON` to build with AddressSanitizer and UBSan (GCC or Clang).

The web app needs Node 24.

```
cd web
npm install
npm run dev
```

`npm run build` writes the static site to `web/dist`.

## License

AGPL-3.0. See [LICENSE](LICENSE).
