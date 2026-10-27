Generic plug-in that treats the VirtualBox mouse device as a touch screen
input. It relies on the VirtualBox guest modules and /dev/vboxguest.

To use this driver, pass -plugin VBoxTouch on the command line.

The driver responds to two environment variables:

 * `VIRTUALBOX_TOUCH_GUEST_DEVICE` (default `/dev/vboxguest`)
 * `VIRTUALBOX_TOUCH_EVDEV_MOUSE` (default `/dev/input/by-path/platform-i8042-serio-1-event-mouse`)

It gets movement events from the guest device and button events from the evdev device. It's not pretty, but that's the way it is.

The devices can also be specified on the command line as part of the plugin
specification.
For example `-plugin VboxTouch:vboxguest=/dev/vboxguest:evdev=/dev/input/mouse0`

The mouse inputs are converted to touchscreen events by reporting only
left button actions (as touches), and reporting movement only when the left
button is down.

## evdev mode

With `mode=evdev` in the specification the driver does not use vboxguest at
all. It reads one or more evdev devices instead, taking positions from those
that report absolute coordinates (VirtualBox's USB tablet or USB multi-touch
device) and buttons from all of them. The devices are given as `/dev/...` or
`evdev=...` entries in the specification, or as a colon-separated list in
`VIRTUALBOX_TOUCH_EVDEV_MOUSE`. Devices that cannot be opened are skipped.

For example `-plugin VboxTouch:mode=evdev:/dev/input/event3:/dev/input/event4`

The zoom gesture (hold Ctrl) works in both modes.

## Tests

`tests/` has unit tests for the handler. They compile the plugin's sources
in, fake the devices with FIFOs and run on the offscreen platform, so they
need neither VirtualBox nor a display:

    cd tests && qmake && make check
