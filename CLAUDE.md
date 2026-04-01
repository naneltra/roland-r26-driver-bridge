CLAUDE.md

Fixing Roland R-26 USB Audio on Modern macOS

Problem

The Roland R-26 sound recorder lost support as a USB audio interface on recent macOS versions. Roland’s last official Mac driver only supports macOS 10.13 High Sierra, and no updates exist for newer macOS releases. Modern macOS changes prevent old kernel drivers from working:
	•	Deprecation of legacy kernel extensions (kexts)
	•	Intel-only signed drivers fail on Apple Silicon
	•	macOS prefers class-compliant USB audio devices

The R-26 uses a vendor-specific USB mode, which macOS rejects without the legacy Roland driver.

Quick Workarounds

1. Use Analog Output
	•	Connect the R-26’s headphone or line-out to a class-compliant USB interface
	•	Use macOS’s aggregate device feature to route audio into DAWs
	•	Works without requiring Roland driver

2. Check USB Audio Settings on R-26
	•	Menu → AUDIO I/F → USB mode / input source
	•	Ensure it is set to transmit stereo audio over USB

3. Use Virtual Audio Bridges
	•	Install BlackHole or Loopback
	•	Route R-26 audio through analog inputs into virtual audio device
	•	Avoids legacy driver entirely

Advanced Options

1. Patching the Roland Driver

Roland’s driver contains:
	•	Old signed kext
	•	Intel-only binary
	•	Obsolete notarization

Challenges:
	•	Apple Silicon requires recompiled drivers
	•	Reverse-engineering USB descriptors required
	•	Writing a fresh kernel driver is high effort

2. User-Space USB Bridge (Recommended)

Instead of kernel driver, create a user-space bridge:

Architecture:

R-26 USB endpoints
      ↓
libusb daemon
      ↓
CoreAudio virtual device
      ↓
Logic / Ableton / Reaper

Advantages:
	•	Avoids kernel signing issues
	•	Works on modern macOS (Intel + Apple Silicon)
	•	Fully user-space, easier to maintain

Implementation Steps:
	1.	Detect Roland R-26 USB device using libusb
	2.	Read PCM audio packets from USB endpoints
	3.	Expose a virtual CoreAudio device using CoreAudio AudioServerPlugIn
	4.	Route audio to DAWs

Next Steps
	1.	Connect R-26 to macOS
	2.	Run:

system_profiler SPUSBDataType

	3.	Record the Vendor ID and Product ID

This information is required to implement the USB bridge or patch driver.

Once collected, a user-space prototype can be developed in C++ or Swift to:
	•	Detect the device
	•	Capture audio packets
	•	Expose a virtual CoreAudio input

This approach is much safer and feasible than writing a new kernel driver from scratch.