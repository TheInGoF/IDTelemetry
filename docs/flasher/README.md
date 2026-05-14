# Web Flasher

Scaffolding for [ESP Web Tools](https://esphome.github.io/esp-web-tools/) — flash the firmware from a browser without any toolchain.

## Files

| File                       | Purpose                                                       |
| -------------------------- | ------------------------------------------------------------- |
| `index.html`               | Landing page with the install buttons                         |
| `manifest_full.json`       | ESP Web Tools manifest for the LILYGO T-SIM7080G-S3 firmware  |
| `manifest_lite.json`       | ESP Web Tools manifest for the plain ESP32-S3 firmware        |

## Hosting

Drop this directory into a GitHub Pages branch (`gh-pages`) or any static host. The `.bin` paths in the manifests are relative to the page and currently point at `../../firmware/` — adjust to match where your release artefacts live (typically a GitHub Release asset URL).

## Building the binaries

```bash
pio run -e s3_full
pio run -e s3_lite

# Artefacts:
# .pio/build/s3_full/firmware.bin
# .pio/build/s3_full/partitions.bin
# .pio/build/s3_full/bootloader.bin
# .pio/build/s3_lite/firmware.bin   (once lite is implemented)
# .pio/build/s3_lite/partitions.bin
# .pio/build/s3_lite/bootloader.bin
```

Rename to match the manifest, upload to the GitHub Release, point the manifest URLs at the asset URLs.

## TODO

- Wire up a GitHub Actions workflow that builds both envs on release-tag push and uploads the renamed `.bin` files as Release assets.
- Update the manifest URLs to point at `https://github.com/<owner>/<repo>/releases/download/<tag>/...`.
- Add a `?serial=ttyACM0` URL parameter for headless flashing demos.
