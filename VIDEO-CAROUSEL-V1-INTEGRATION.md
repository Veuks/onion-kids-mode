# Integrating VideoCarousel V1

Base tested for patch generation: `Veuks/onion-kids-mode` commit `23603ed`.

Either extract the source bundle at the repository root or apply the patch:

```sh
git apply --check VideoCarousel-V1.patch
git apply VideoCarousel-V1.patch
```

Commit the added files and the `.github/workflows/build.yml` change, then run
the `Build` workflow. The workflow cross-compiles both new binaries and puts
them under `App/VideoCarousel/bin/` in the downloadable artifact.

No existing file below `App/KidsMode/` or `src/kidsMode/` is modified.
