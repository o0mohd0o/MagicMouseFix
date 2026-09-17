<p align="center"><img src="assets/icon-1024.png" width="160" alt="MagicMouseFix icon"></p>

# MagicMouseFix

A tiny fix for the Apple Magic Mouse going dead after upgrading to **macOS 27**.

The mouse pairs, shows **Connected**, even reports its battery level, but the cursor
never moves. Nothing is wrong with the mouse. Apple's driver stalls during startup, and
one missing message is all it takes to get it going again. MagicMouseFix sends that
message. After that the mouse works **natively**: pointer, clicks, scrolling and
gestures all come from Apple's own driver, exactly as before.

> Not affiliated with or endorsed by Apple Inc. Magic Mouse and macOS are trademarks of
> Apple Inc.

## Is this your problem?

- You upgraded to macOS 27 and the Magic Mouse stopped working; it was fine on macOS 26.
- Bluetooth shows it as connected, with a battery percentage.
- It fails the same way over Bluetooth **and** with the cable plugged in.
- Re-pairing, rebooting and Safe Mode do not help.
- A Magic Trackpad on the same Mac works normally.

You can confirm it in Terminal. With the mouse connected, this prints nothing when you
are affected:

```sh
hidutil list | grep -i 0x269 | grep AppleMultitouchDevice
```

## Install

1. Download `MagicMouseFix-x.y.z.zip` from [Releases](../../releases) and unzip it.
2. Move **MagicMouseFix.app** to your **Applications** folder.
3. Open it. It installs a small login agent and asks for one permission.
4. In **System Settings > Privacy & Security > Input Monitoring**, turn on
   **MagicMouseFix**.

The mouse should start working within a few seconds. From then on the agent runs at
login and wakes the mouse every time it connects.

The app is signed with a Developer ID and **notarized by Apple**, so it opens without
Gatekeeper warnings. You can also build it yourself, below.

### About the Input Monitoring permission

macOS files *all* access to keyboards and mice under "Input Monitoring", including
writing to them. MagicMouseFix only **writes** one 4-byte message to the mouse. It does
not read your keystrokes or mouse input, does not move the cursor, and makes no network
connections. The whole program is one short C file:
[`src/MagicMouseFix.c`](src/MagicMouseFix.c).

## Uninstall

```sh
/Applications/MagicMouseFix.app/Contents/MacOS/MagicMouseFix --uninstall
```

Then delete the app. Once Apple ships a fixed driver you will not need it any more. It
is harmless to leave installed in the meantime: when the driver comes up by itself, the
agent sees that and does nothing.

## What is actually wrong

When a Magic Mouse connects, `AppleMultitouchMouseHIDEventDriver` starts and then runs an
`Enumerate MT` step that creates the `AppleMultitouchDevice` all pointer input flows
through. On macOS 27.0 that step never runs for the mouse:

| | driver starts | `Enumerate MT` |
|---|---|---|
| macOS 26 | yes | yes, every connect |
| macOS 27.0 | yes, "Successfully started" | **never** |

`bluetoothd` logs every HID report it sends to a device, payload included. Comparing a
working macOS 26 connect with a broken macOS 27 one shows the difference. On macOS 26
the driver opens with three writes to the mouse:

```
F1 06 01 37
F1 01 DB
F1 02 01      <- enables multitouch (the same command Linux's hid-magicmouse uses)
```

On macOS 27 none of them are sent to the mouse, although the Magic Trackpad still gets
its equivalents and works. The driver is left waiting. Sending just the first message,
`F1 06 01 37`, is enough: the driver runs `Enumerate MT` about 11 ms later and then
completes the rest of its own setup.

```
22:31:36.055  bluetoothd   Set report {... 'F1 06 01 37'}      <- MagicMouseFix
22:31:36.066  kernel       AppleMultitouchHIDEventDriverV2 ... Enumerate MT
22:31:36.404  kernel       AppleMultitouchDevice::start ... Successfully started
```

You can watch the same thing on your Mac:

```sh
/usr/bin/log stream --info --debug --predicate \
  'sender == "AppleMultitouchDriver" OR (process == "bluetoothd" AND eventMessage CONTAINS "Set report")'
```

### What the agent does

On every mouse connect it waits a moment, checks the I/O Registry for the mouse's
`AppleMultitouchDevice`, and only if it is missing sends feature report `0xF1` with
payload `06 01 37`. It re-checks and retries every two seconds, up to ten times. Its log
is at `~/Library/Logs/MagicMouseFix.log`.

## Tested on

- Magic Mouse 2 (Lightning), product ID `0x0269`, firmware 1.9.2
- macOS 27.0 (26A428), Apple silicon

Other Magic Mouse models are not matched, because the fix has not been verified on them.
If you have one that is affected, please open an issue with the output of
`hidutil list | grep -i mouse`.

## Build from source

Needs only the Xcode Command Line Tools (`xcode-select --install`).

```sh
scripts/build.sh                     # ad-hoc signed, for your own Mac
SIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" scripts/build.sh
scripts/notarize.sh                  # optional, see the script header
```

An ad-hoc build works, but macOS ties the Input Monitoring permission to the exact
binary, so you must grant it again after every rebuild. A Developer ID signature avoids
that.

## Fallback: driving the cursor in user space

[`extras/cursor-driver.c`](extras/cursor-driver.c) is an earlier approach, kept for
reference. It reads the mouse's raw HID report `0x12` (buttons plus 16-bit relative X/Y)
and synthesizes cursor movement and clicks itself. It works without the native driver,
but it cannot scroll and needs the Accessibility permission as well. Waking the real
driver is better in every way.

## License

MIT. See [LICENSE](LICENSE).
