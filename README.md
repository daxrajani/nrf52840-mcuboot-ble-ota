# Secure A/B OTA Bootloader — nRF52840 DK

A production-quality reference implementation of secure firmware OTA for the nRF52840 DK using:

- **MCUboot** (swap-scratch, A/B slots) as the bootloader
- **ECDSA P-256** image signing — MCUboot rejects any image not signed with the project key
- **MCUmgr SMP over BLE** for wireless firmware upload (compatible with nRF Connect mobile app)
- **Zephyr NCS v3.2.4** with sysbuild (MCUboot + application built together)
- **Automatic image confirmation** with rollback on failure

---

## How It Works

```
┌─────────────┐   BLE/SMP    ┌────────────────────┐
│ nRF Connect │ ──upload───▶ │  Slot 1 (secondary) │
│ mobile app  │              │  zephyr.signed.bin  │
└─────────────┘              └────────┬───────────┘
                                      │ reset
                             ┌────────▼───────────┐
                             │      MCUboot        │
                             │  verify ECDSA sig   │
                             │  swap-scratch A/B   │
                             └────────┬───────────┘
                                      │ chainload
                             ┌────────▼───────────┐
                             │  Slot 0 (primary)   │
                             │  new firmware       │
                             │  confirms itself    │  ← boot_write_img_confirmed()
                             │  at t ≈ 2 s         │
                             └────────────────────┘
```

If the new image fails to confirm (crash, watchdog, or `CRASH_DEMO` build flag), MCUboot reverts to the previous image on the next boot. Confirmation only runs when `mcuboot_swap_type() == BOOT_SWAP_TYPE_REVERT`, so it is a true no-op on normal boots.

---

## Flash Layout (nRF52840 — 1 MB)

| Region | Start | Size | Purpose |
|--------|-------|------|---------|
| `mcuboot` | `0x00000` | 48 KB | Bootloader |
| `mcuboot_primary` | `0x0C000` | 428 KB | Slot 0: running image + MCUboot pad |
| `mcuboot_secondary` | `0x77000` | 428 KB | Slot 1: OTA upload target |
| `mcuboot_scratch` | `0xE2000` | 112 KB | Scratch buffer (power-loss-safe swap) |
| `settings_storage` | `0xFE000` | 8 KB | NVS: BLE pairing keys, identity |

Partition layout is pinned in `app/pm_static_nrf52840dk_nrf52840.yml` to prevent NCS auto-PM from shifting regions across SDK upgrades.

---

## Prerequisites

- **nRF Connect for Desktop** with the **Toolchain Manager** (installs NCS v3.2.4 + Zephyr SDK)
- **nRF52840 DK** connected via USB (JLink)
- **nRF Connect** mobile app (iOS or Android) for BLE OTA
- A **serial terminal** at 115200 baud for UART logs (the DK enumerates a JLink CDC COM port)

> All build commands below must be run from an **NCS terminal** launched by Toolchain Manager. It pre-sets `ZEPHYR_BASE`, `ZEPHYR_SDK_INSTALL_DIR`, and the full toolchain PATH.

---

## Repository Setup (one-time)

> **Step 0 — generate your signing key first.** The build will fail with a CMake fatal error if `app/keys/mcuboot.pem` does not exist. Run the key generation script before anything else.

```powershell
# From the repo root
west update
west zephyr-export
.\scripts\setup_dev_keys.ps1
```

```bash
# Linux / macOS
west update
west zephyr-export
./scripts/setup_dev_keys.sh
```

`setup_dev_keys` generates `app/keys/mcuboot.pem` (ECDSA P-256 private key) using `imgtool keygen`, falling back to OpenSSL if imgtool is not on PATH. **This key is the root of trust — back it up and never commit it to version control.**

Manual key generation (if scripts are unavailable):

```bash
openssl ecparam -name prime256v1 -genkey -noout -out app/keys/mcuboot.pem
```

---

## Building Firmware

Version and BLE name are passed via **environment variables** because the application cmake runs as a sysbuild `ExternalProject` subprocess and does not inherit cmake `-D` arguments from the top-level invocation. Environment variables are inherited by subprocess cmake.

### Build v1.0.0 (baseline — flash to device)

```powershell
# Windows (PowerShell / NCS terminal)
$env:APP_FIRMWARE_VERSION="1.0.0"; $env:APP_BLE_NAME="Dax_BLE_v1"
west build --sysbuild -b nrf52840dk/nrf52840 app -d build_v100 -p always
```

```bash
# Linux / macOS
APP_FIRMWARE_VERSION=1.0.0 APP_BLE_NAME=Dax_BLE_v1 west build --sysbuild -b nrf52840dk/nrf52840 app -d build_v100 -p always
```

### Build v2.0.0 (OTA target)

```powershell
$env:APP_FIRMWARE_VERSION="2.0.0"; $env:APP_BLE_NAME="DAX_OTA_V2"
west build --sysbuild -b nrf52840dk/nrf52840 app -d build_v200 -p always
```

```bash
APP_FIRMWARE_VERSION=2.0.0 APP_BLE_NAME=DAX_OTA_V2 west build --sysbuild -b nrf52840dk/nrf52840 app -d build_v200 -p always
```

Use the same pattern for any version. The version string is embedded in the binary, passed to `imgtool sign`, and printed by the app at boot.

---

## Flashing

```powershell
# Full chip erase + flash (first time or after key change)
west flash -d build_v100 --runner nrfjprog -- --erase

# Normal reflash (preserves NVS / BLE bonding data)
west flash -d build_v100 --runner nrfjprog
```

`build_v100/merged.hex` includes both MCUboot and the application. Always flash the `merged.hex` from the sysbuild output directory — never flash the application `.hex` alone, as that would leave MCUboot out of sync.

---

## UART Logs

Install pyserial if not already present:

```bash
pip install pyserial
```

Find the JLink CDC COM port:

```powershell
# Windows
Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Name
```

```bash
# Linux / macOS
ls /dev/tty.usbmodem* /dev/ttyACM* 2>/dev/null
```

Open terminal (115200 8N1):

```bash
python -m serial.tools.miniterm <PORT> 115200
```

Expected output on a clean v1.0.0 boot:

```
*** Booting MCUboot v2.3.0-dev ***
I: Image version: v1.0.0
[00:00:00.054] secure_ble_ota: MCUboot-aware firmware v1.0.0
[00:00:00.216] secure_ble_ota: Advertising as 'Dax_BLE_v1' (SMP / image upload enabled)
[00:00:02.226] secure_ble_ota: image confirm skipped — swap_type=1 (normal boot, no pending revert)
```

---

## OTA Update Flow

### Upload via nRF Connect mobile app

1. Flash `build_v100/merged.hex` (v1.0.0 base).
2. Open **nRF Connect** → scan → connect to `Dax_BLE_v1`.
3. Go to **DFU** tab → select `build_v200/app/zephyr/zephyr.signed.bin`.
4. Tap **Open** then **Start** — the upload takes ~30–60 seconds over BLE.
5. The device resets automatically when the upload completes (no physical reset needed).
6. MCUboot validates the ECDSA P-256 signature, performs the swap, and boots v2.0.0.
7. The app confirms the image at t ≈ 2 s.

### Expected UART output (full successful OTA)

```
# --- Running v1.0.0, upload starts ---
secure_ble_ota: DFU_STARTED — slot-1 upload begun
secure_ble_ota: DFU_PENDING — slot-1 upload complete; image test-pending set
secure_ble_ota: MCUmgr OS RESET (SMP) force=0 delay_ms=250 -> warm reboot (SREQ)

# --- MCUboot performs swap ---
I: Image index: 0, Swap type: test
I: Starting swap using scratch algorithm.
I: Image version: v2.0.0

# --- v2.0.0 confirms itself ---
secure_ble_ota: MCUboot-aware firmware v2.0.0
secure_ble_ota: Advertising as 'DAX_OTA_V2' (SMP / image upload enabled)
[00:00:02.xxx] secure_ble_ota: test swap detected — boot_write_img_confirmed() writing image_ok flag
secure_ble_ota: image confirmed OK
```

### Downgrade / rollback via nRF Connect

After a v1→v2 swap, MCUboot moves the old v1 image to slot 1. nRF Connect can detect this and mark it as test without re-uploading. The app allows this reset automatically (`mcuboot_swap_type() == BOOT_SWAP_TYPE_TEST` check in the reset guard). The flow is identical to an upgrade.

---

## Rollback Demo (CRASH_DEMO)

Build with the crash flag to test MCUboot's automatic revert:

```powershell
$env:APP_FIRMWARE_VERSION="2.0.0"; $env:APP_BLE_NAME="DAX_OTA_V2"
west build --sysbuild -b nrf52840dk/nrf52840 app -d build_v200_crash -p always -- -DCRASH_DEMO=1
```

OTA with this binary. The new image will `k_panic()` before calling `boot_write_img_confirmed()`. MCUboot detects the unconfirmed test image on the next boot and reverts to the previous version automatically.

---

## Verifying a Build

After building, confirm the correct values were embedded:

```powershell
# Windows
Select-String "APP_FIRMWARE_VERSION|APP_BLE_NAME" .\build_v200\app\CMakeCache.txt
Select-String "SIGNATURE_KEY" .\build_v200\app\zephyr\.config
Select-String "BOOT_SIGNATURE_KEY" .\build_v200\mcuboot\zephyr\.config
```

```bash
# Linux / macOS
grep -E "APP_FIRMWARE_VERSION|APP_BLE_NAME" build_v200/app/CMakeCache.txt
grep "SIGNATURE_KEY" build_v200/app/zephyr/.config
grep "BOOT_SIGNATURE_KEY" build_v200/mcuboot/zephyr/.config
```

Both key paths must point to `app/keys/mcuboot.pem`. A mismatch means MCUboot will reject the OTA image at signature verification.

Verify the signed binary directly:

```bash
imgtool verify -k app/keys/mcuboot.pem build_v200/app/zephyr/zephyr.signed.bin
```

---

## OTA Binary Reference

| File | Purpose |
|------|---------|
| `build_vXXX/merged.hex` | Full flash image (MCUboot + app) — use with `west flash` |
| `build_vXXX/app/zephyr/zephyr.signed.bin` | OTA payload — upload via nRF Connect mobile app |
| `build_vXXX/dfu_application.zip` | MCUmgr-compatible DFU package (alternative to raw `.bin`) |

---

## Engineering Decisions & Challenges

**Static partition map instead of NCS auto-PM.**
NCS's partition manager recalculates region addresses whenever `CONFIG_*` options change the MCUboot binary size. That is safe during development but dangerous in production — a routine Kconfig change can silently shift the secondary slot address, causing every field-deployed device to refuse the next OTA because the upgrade lands at the wrong offset. `pm_static_nrf52840dk_nrf52840.yml` locks all addresses, trading flexibility for predictability. Any future resizing requires an explicit, reviewed change to the partition file.

**Sysbuild ExternalProject cmake isolation.**
In Zephyr sysbuild, the application cmake runs as a separate `ExternalProject` subprocess. cmake `-D` arguments passed on the west command line are visible only to the top-level sysbuild cmake — they never reach the application cmake. The standard workaround (pass `-DAPP_FIRMWARE_VERSION=2.0.0` after `--`) silently fails; the application always sees the default. The fix is to read `$ENV{APP_FIRMWARE_VERSION}` inside the application `CMakeLists.txt`, because environment variables are inherited by subprocess cmake. Similarly, `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` must be injected via `SB_EXTRA_CONF_FILE` *before* `find_package(Sysbuild)` — setting it afterwards is too late because `ExternalZephyrProject_Cmake()` runs inside `find_package` and has already written the child image Kconfig by the time user code after `find_package` executes.

**OTA reset guard for premature mobile resets.**
Several BLE DFU client implementations (including nRF Connect on some iOS versions) send an `os_mgmt` reset command immediately after connecting, before the image upload has started. Without a guard, this resets the device at 0% transfer. The guard in `main.c` blocks `os_mgmt` resets unless `img_dfu_active` (upload in progress), `img_dfu_pending` (upload just finished), or `mcuboot_swap_type() == BOOT_SWAP_TYPE_TEST` (slot 1 already has a pending image — the downgrade case where nRF Connect re-marks an existing slot-1 image as test via state_write without re-uploading). The third condition was discovered when testing downgrade: `DFU_STARTED`/`DFU_PENDING` events never fire for state_write-only flows, so flags alone are insufficient.

**Image confirmation timing with active BLE connection.**
After MCUboot test-swaps a new image, the nRF Connect app reconnects immediately to verify the version string. If image confirmation is deferred until after disconnect (a common pattern), `boot_write_img_confirmed()` never runs before the next reset and MCUboot reverts the image. The fix is to confirm while connected — the nRF52840 SoftDevice Controller handles flash/radio arbitration in hardware, so a single-byte `image_ok` write is safe during an active BLE connection. A 2-second initial delay protects the first connection window; after that, confirmation proceeds regardless of connection state.

**Key propagation through the signing chain.**
MCUboot is compiled with a public verification key. The OTA binary is signed with the corresponding private key via `imgtool`. If these keys diverge — as happens when the NCS default key (`root-ec-p256.pem`) is used for signing but a custom key was compiled into MCUboot, or vice versa — MCUboot silently discards the slot-1 image rather than logging a swap failure, making the failure mode appear identical to "no image uploaded." The fix is a single source of truth: `app/keys/mcuboot.pem` is referenced by both the app cmake (for `imgtool sign`) and the sysbuild cmake (for `CONFIG_BOOT_SIGNATURE_KEY_FILE` in MCUboot). Any key change requires a full rebuild and reflash of both images.

---

## Repository Layout

```
├── west.yml                               # NCS v3.2.4 workspace manifest
├── app/
│   ├── CMakeLists.txt                     # Build logic: version/name env vars, key path, EXTRA_CONF_FILE
│   ├── prj.conf                           # Application Kconfig (BLE, MCUmgr, SMP, hooks, flash)
│   ├── sysbuild.conf                      # Sysbuild: enable MCUboot, swap-scratch, ECDSA P-256
│   ├── sysbuild/
│   │   ├── CMakeLists.txt                 # Key path injection via SB_EXTRA_CONF_FILE (before find_package)
│   │   └── mcuboot.conf                   # MCUboot child-image config (UART logging, signature type)
│   ├── pm_static_nrf52840dk_nrf52840.yml  # Static partition map (pinned addresses)
│   ├── keys/
│   │   └── mcuboot.pem                    # ECDSA P-256 signing key (gitignored — generate locally)
│   └── src/
│       └── main.c                         # BLE peripheral, SMP, DFU hooks, image confirmation
└── scripts/
    ├── setup_dev_keys.ps1                 # Key generation (Windows/PowerShell)
    └── setup_dev_keys.sh                  # Key generation (Linux/macOS)
```

---

## CI

GitHub Actions (`.github/workflows/build.yml`) runs on every push to `main`/`master` and on pull requests:

1. Installs Zephyr SDK 0.16.8 (ARM) and west/imgtool
2. Runs `west init -l . && west update`
3. Generates an **ephemeral** ECDSA P-256 key (not the dev key)
4. Builds `west build --sysbuild -b nrf52840dk/nrf52840`
5. Verifies the signed binary: `imgtool verify -k app/keys/mcuboot.pem build/app/zephyr/zephyr.signed.bin`

CI validates that the signing pipeline is intact. It does not run hardware tests.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `Missing mcuboot.pem` cmake fatal error | Key not generated | Run `setup_dev_keys.ps1` / `setup_dev_keys.sh` before building |
| `west: command not found` | Not in NCS terminal | Launch terminal from Toolchain Manager |
| No UART output after flash | Flashed app-only hex | Always flash `build_vXXX/merged.hex` via `west flash -d build_vXXX` |
| `E: Image in the secondary slot is not valid!` | Key mismatch between MCUboot and OTA binary | Rebuild both base and OTA firmware; verify `BOOT_SIGNATURE_KEY_FILE` matches in both `.config` files |
| Upload completes but no swap, no error logged | Stale build used wrong NCS default key | Delete all `build_*/` dirs; rebuild after running key setup script |
| Device resets immediately at 0% upload | nRF Connect sends early OS reset | Handled automatically by the reset guard in `main.c` |
| Physical reset required after OTA | Old firmware without `img_dfu_pending` fix | Rebuild from current source |
| Downgrade reset blocked | No DFU upload, only state_write | Handled by `mcuboot_swap_type() == BOOT_SWAP_TYPE_TEST` check in reset guard |
| Version always shows `1.0.0` | Env var not set before build | Set `$env:APP_FIRMWARE_VERSION` (PowerShell) or `APP_FIRMWARE_VERSION=x.y.z` (bash) before `west build` |

---

## Security Notes

- `app/keys/mcuboot.pem` is listed in `.gitignore` and must never be committed. It is the only key MCUboot will accept for image validation.
- For production, generate the signing key on an HSM or air-gapped machine. The CI pipeline uses an ephemeral key; replace it with your production key management workflow before shipping.
- `CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE=y` is set for development convenience. Disable it in production to prevent unauthenticated pairing overwrites.
