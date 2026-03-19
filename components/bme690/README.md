# BME690 (vendored, patched)

This directory is copied from [ESPHome PR #13480](https://github.com/esphome/esphome/pull/13480) (BSEC3 + ESP-IDF).

## Patch vs upstream

- **`__init__.py`**: `esp32.only_on_variant` is changed from **ESP32-C6 only** to **ESP32-C3, ESP32-C6, and ESP32-S3**, so validation matches common boards.

You must still supply a Bosch **`libalgobsec.a`** built for **your exact target** (e.g. `esp32c3` vs `esp32s3` are different — a C3 library will not link on S3).

## Updating from upstream

Re-download the PR branch files and re-apply the `only_on_variant` change in `__init__.py`, or diff against this repo.
