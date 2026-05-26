# Updating ezinject

[ezinject](https://github.com/smx-smx/ezinject) is the binary that injects the PHP hook into `lginput2` and other targets. It is vendored in `service/inputhook/ezinject`.

## Does updating fix the "remote stops working" crash?

**No.** That crash (issue #23) was a null pointer dereference *inside* the PHP hook when lginput2 passed null pointers during device teardown. The fix is defensive null checks in `lginput-hook.php`, not ezinject.

Updating ezinject can still improve **general injection stability** (upstream has fixes for stack handling, error paths, and memory leaks) and ensures you use the **correct architecture** for your TV (see below).

## Architecture

- **32-bit ARM** (armv7): use `arm` or `armhf` build from ezinject.
- **64-bit ARM** (aarch64): use `aarch64` or `arm64` build. Many newer webOS TVs are aarch64; the crash dump in issue #23 was aarch64.

The binary in the repo may be 32-bit ARM. Check with:

```bash
file service/inputhook/ezinject
```

If your TV is 64-bit, you need an aarch64 ezinject (build from upstream or obtain a prebuilt one).

## How to build ezinject for webOS

1. Clone ezinject and build for the target architecture (cross-compile or use a device/sysroot that matches webOS):

   ```bash
   git clone https://github.com/smx-smx/ezinject.git
   cd ezinject
   mkdir build && cd build
   ```

   For **aarch64** (64-bit webOS):

   ```bash
   cmake .. -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_SYSTEM_NAME=Linux \
     -DCMAKE_SYSTEM_PROCESSOR=aarch64
   # If you have a cross-toolchain:
   # -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc
   make
   ```

   For **32-bit ARM** (armv7):

   ```bash
   cmake .. -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_SYSTEM_NAME=Linux \
     -DCMAKE_SYSTEM_PROCESSOR=arm
   make
   ```

2. The PHP loader is in `samples/php`. You need the **ezinject** binary from the build directory (e.g. `build/ezinject`). The lginputhook app uses ezinject to load `libphp.so` (from libcrypt1 or libcrypt2) and run `lginput-hook.php`; it does not use ezinject’s PHP sample binary directly, only the injection tool.

3. Copy the binaries into `service/inputhook/` with architecture-specific names:

   ```bash
   # 32-bit ARM
   cp /path/to/ezinject/build/ezinject /path/to/lginputhook/service/inputhook/ezinject-arm

   # 64-bit ARM (aarch64)
   cp /path/to/aarch64/ezinject/build/ezinject /path/to/lginputhook/service/inputhook/ezinject-aarch64

   chmod 755 /path/to/lginputhook/service/inputhook/ezinject-*
   ```

   On your TV `tvlorkaeth.lan` we detected `uname -m` = `aarch64`, so it will use `ezinject-aarch64`.

4. Rebuild and repackage the app (`npm run build && npm run package`), then reinstall on the TV.

## References

- [smx-smx/ezinject](https://github.com/smx-smx/ezinject)
- [ezinject PHP sample](https://github.com/smx-smx/ezinject/tree/master/samples/php)
- [Issue #23 – Remote stops working](https://github.com/Simon34545/lginputhook/issues/23) (fixed by null checks in `lginput-hook.php`)
