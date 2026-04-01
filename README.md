Architecture

  R-26 (USB, Vendor ID 0x0582, Product ID 0x013E)
        ↓ isochronous USB audio
  r26d daemon (libusb)
        ↓ POSIX shared memory ring buffer (/r26audio)
  R26Audio.driver (CoreAudio AudioServerPlugIn)
        ↓
  Logic / Ableton / Reaper / any macOS app

  Components

  1. src/shared/RingBuffer.h — Lock-free SPSC ring buffer via POSIX shared memory, used by both components
  2. src/daemon/ (r26d) — libusb daemon that detects the R-26, reads isochronous audio packets, converts 24-bit PCM
  to float, writes to shared memory
  3. src/driver/ (R26Audio.driver) — CoreAudio AudioServerPlugIn that reads from shared memory and appears as
  "Roland R-26" input device in macOS
  4. scripts/install.sh / scripts/uninstall.sh — Install/remove both components

  How to use

```bash
cd build && make -j$(sysctl -n hw.ncpu) && sudo cp r26d /usr/local/bin/r26d && sudo r26d
```


  1. Install:
  sudo ./scripts/install.sh
  1. This copies the driver to /Library/Audio/Plug-Ins/HAL/ and the daemon to /usr/local/bin/.
  2. Restart CoreAudio:
  sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod
  3. Connect R-26 (set to USB Audio Interface mode: Menu → AUDIO I/F)
  4. Probe first (to see USB descriptors and verify detection):
  sudo r26d --probe
  5. Start capturing:
  sudo r26d
  6. Select "Roland R-26" as input in Audio MIDI Setup or your DAW.

  Important notes

  - The original driver is an x86_64-only kernel extension — it cannot work on macOS 15.6 (kexts are deprecated, no
  ARM64 support)
  - This user-space bridge bypasses all kernel signing/notarization issues
  - The --probe flag is critical for the first run — it dumps the R-26's actual USB descriptors so we can verify the
   endpoint layout and packet format
  - If the audio format doesn't match the defaults (48kHz/24-bit/stereo), the probe output will reveal the actual
  parameters, and the daemon's format detection can be refined

  ```

            R-26(AUDIO):

              Product ID: 0x013e
              Vendor ID: 0x0582  (Roland Corporation)
              Version: 0.00
              Speed: Up to 480 Mb/s
              Manufacturer: Roland
              Location ID: 0x02110000 / 8
              Current Available (mA): 500
              Extra Operating Current (mA): 0
```