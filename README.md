# btkbd: Bluetooth LE keyboards on old Macs, via a $10 USB dongle

**A from-scratch, user-space Bluetooth Low Energy keyboard driver for macOS**, for Macs whose built-in Bluetooth
is too old for BLE (Bluetooth 2.1 / 3.0 Macs from ~2008–2011) and for Realtek USB dongles that macOS has no
driver for.

No kernel extension. System Integrity Protection stays on. Works at the **login window** (so you can type your
password with the keyboard) and comes back by itself after every reboot.

Tested on a 2009 iMac running **macOS 10.13.6 High Sierra** with a **TP-Link UB500** dongle (Realtek RTL8761B) and a
BiTech BY-WS-111 BLE keyboard.

---

## Why this exists

Many cheap modern keyboards are **Bluetooth LE only** (HID over GATT). Older Macs can't use them:

- The built-in Bluetooth 2.1 radio has no Low Energy at all (`System Information → Bluetooth → Bluetooth Low Energy
  Supported: No`).
- The usual fix, a USB dongle, only works if macOS has a driver for its chip. Apple ships drivers for Broadcom only.
  Today's cheap dongles (TP-Link UB500, ASUS BT500 and many no-name sticks) use the **Realtek RTL8761B**, and macOS
  ignores them completely.

btkbd talks to the Realtek dongle directly over USB and runs the whole Bluetooth LE stack itself: firmware, pairing,
encryption and the HID-over-GATT keyboard profile. It then types the keys into macOS.

## Features

- Loads the RTL8761B firmware into the dongle (the same procedure as Linux `btrtl`)
- Bluetooth LE pairing (SMP, LE legacy: Just Works or passkey) with bonding: the keys are saved, and reconnects are encrypted
- Full HID-over-GATT client: finds the keyboard's reports, reads its HID report descriptor and decodes it, so it isn't
  hard-coded to one keyboard
- Every key, modifiers (left and right), key repeat at your System Preferences speed, and **Caps Lock with its LED** on the keyboard
- Media keys: volume, mute, play/pause, next/previous, stop
- App keys: media player (iTunes), browser home, mail (opens Gmail), "My Computer" (Finder), files (home folder)
- **Mac-style function row**: the media/app actions are the default and **Fn + F-key** gives F1–F12. To get standard
  F-keys back: `sudo touch /usr/local/btkbd/standard-fkeys` and log out and back in.
- Starts at boot (root LaunchDaemon), restarts itself if anything goes wrong, and reconnects whenever the keyboard wakes up
- Works at the login window, in password fields and with Secure Keyboard Entry
- Meant to survive unplugging and replugging the dongle: the service exits, launchd restarts it and it loads the firmware again (not tested yet)

## How it works

```
 BLE keyboard  ))))  RTL8761B USB dongle
                          │  USB (bulk + interrupt endpoints, HCI over IOKit, no kext)
                          ▼
 ┌─────────────────────────────────────────────────────────────┐
 │ btkbd  (root LaunchDaemon, starts at boot)                   │
 │  hci.c       USB HCI transport + Realtek firmware loader     │
 │  btkbd.c     scan, connect, SMP pairing, LE encryption,      │
 │              L2CAP, ATT/GATT, HID-over-GATT reports          │
 │  hidparse.c  HID report descriptor → pressed keys            │
 └───────────────┬─────────────────────────────────────────────┘
                 │  key down / up over a local Unix socket
                 │  (/var/run/btkbd.sock, only root or the console user may connect)
                 ▼
 ┌─────────────────────────────────────────────────────────────┐
 │ btkbd-inject (LaunchAgent: login window + every user session)│
 │  CGEventPost key events, media keys, key repeat,            │
 │  Caps Lock via IOHIDSystem. Only the session that owns the  │
 │  screen types, so keys never arrive twice.                  │
 └─────────────────────────────────────────────────────────────┘
```

**Why two programs?** The cleanest design would be a virtual HID keyboard (`IOHIDUserDevice`), so macOS sees a real
keyboard. On High Sierra the kernel refuses it (`IOHIDFamily: btkbd is not entitled`): it needs the
`com.apple.hid.manager.user-access-device` entitlement, and AMFI kills any program that isn't Apple-signed and claims
it. So the root service handles Bluetooth, and a small helper in each GUI session (the login window too) posts the
keystrokes. That's the same approach remote-control apps use at the login screen.

**Why no kernel extension?** Nothing on macOS claims the dongle's USB interfaces, so a normal program can open them
through IOKit's user-space USB API and speak HCI directly (commands as control transfers, events on the interrupt
endpoint, ACL data on the bulk endpoints). One trick: interfaces that no driver ever matched are left "unregistered" and
can't be opened, so btkbd sets the device to configuration 0 and back to 1, which makes macOS register them.

## Supported hardware

| Dongle | USB ID | Status |
|---|---|---|
| TP-Link UB500 | `2357:0604` | tested |
| ASUS USB-BT500 | `0b05:190e` | should work (same chip) |
| Edimax BT-8500 | `7392:c611` | should work (same chip) |
| Realtek RTL8761BU generic | `0bda:8771`, `2550:8761` | should work (same chip) |

Other RTL8761B/BU sticks: run with `BTKBD_USB=vvvv:pppp`. Other Realtek chips need different firmware and aren't supported.

Keyboards: any BLE (HID over GATT) keyboard that accepts **LE legacy pairing**, which is most of them. Keyboards that
insist on LE Secure Connections only will refuse to pair (the log says so).

macOS: written for and tested on 10.13 High Sierra. It uses only long-standing public APIs, so it should build on
10.11–10.14 too (untested). Newer macOS versions can usually use these dongles in other ways.

## Install

You need the Command Line Tools (`xcode-select --install`).

```sh
git clone https://github.com/datbluepony/btkbd-macos.git
cd btkbd-macos
make                 # builds build/btkbd and build/btkbd-inject
sudo make install    # downloads the Realtek firmware from linux-firmware, installs the service + helper
```

Pair the keyboard (once):

```sh
# put the keyboard in pairing mode (usually: hold its Bluetooth / channel key until the light blinks fast)
sudo /usr/local/btkbd/btkbd pair            # or: sudo /usr/local/btkbd/btkbd pair "name filter"
sudo launchctl load -w /Library/LaunchDaemons/local.btkbd.plist
```

If the keyboard wants a passkey, btkbd prints a 6-digit number. Type it on the keyboard and press Enter.
Then press any key: the keyboard connects within a second and works from then on, including after reboots and at the login window.

## Everyday use

| | |
|---|---|
| Log | `tail -f /usr/local/btkbd/btkbd.log` |
| Re-pair (new keyboard or reset keyboard) | `sudo launchctl unload /Library/LaunchDaemons/local.btkbd.plist`, `sudo /usr/local/btkbd/btkbd pair`, load it again |
| Standard F-keys instead of media keys | `sudo touch /usr/local/btkbd/standard-fkeys`, then log out and back in |
| Uninstall | `sudo make uninstall` |
| Debug HCI traffic | stop the service, then `sudo V=1 /usr/local/btkbd/btkbd` |

The saved pairing is in `/usr/local/btkbd/bond.txt` (root only, mode 600).

## Troubleshooting

- **"no RTL8761B Bluetooth dongle found"**: check `system_profiler SPUSBDataType` for the dongle's USB ID, then set `BTKBD_USB`.
- **"encryption failed (0x06)"**: the keyboard forgot the pairing (reset, or paired to another computer on the same
  channel). Pair again.
- **Nothing types, but the log says "keyboard ready"**: check that the helper runs: `cat /tmp/btkbd-inject.log` should say
  `connected to btkbd`. If it doesn't, log out and back in, or run `sudo make install` again.
- **Keys work in your session but not at the login window**: the helper is a `LoginWindow` + `Aqua` LaunchAgent. Make sure
  `/Library/LaunchAgents/local.btkbd-inject.plist` exists, then reboot once.
- **FileVault**: the pre-boot FileVault unlock screen runs before macOS starts, so no add-on driver can work there. Use
  a USB keyboard for that screen, or turn FileVault off.

## Limitations

- One keyboard at a time.
- LE legacy pairing only (no LE Secure Connections yet; the dongle supports the needed P-256 commands, so it's doable).
- Fn and the media keys are mapped for keyboards that send the standard HID consumer codes. The Fn swap table is
  `FN_CODES` in `src/inject.m`.
- Mice and other HID devices aren't handled (the GATT side would work; the helper only types keys).

## Credits

- The Realtek firmware download format and procedure follow the Linux kernel's `drivers/bluetooth/btrtl.c`.
- Firmware comes from the official [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git)
  repository and is downloaded at install time, never redistributed here.
- Pairing crypto (c1, s1, ah) follows the Bluetooth Core Specification, Vol 3 Part H, built on CommonCrypto's AES-128.

## License

MIT, see [LICENSE](LICENSE).
