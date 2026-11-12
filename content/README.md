# Content recipes

Factory wavetables and textures are not stored as audio. Each one is a short JSON recipe that
says how to make it; `zederia-cli content build` turns the recipes into WAV files:

```
zederia-cli content build --in content --out build/factory
```

writes `Wavetables/<Category>/<Name>.wav` and `Textures/<Name>.wav` under the output folder.
Building is deterministic: the same recipe always gives the same file. The step and layer types
are listed in `core/include/zederia/content/Recipe.h`.

This is the starter set of milestone 2 (12 wavetables, 6 textures), chosen to exercise every
part of the toolkit. The full factory set arrives with milestone 7.
