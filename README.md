# mutter-virtual-monitor

`vmon` adds a virtual monitor to a GNOME Wayland session and keeps it for as
long as it runs. The monitor behaves like a real one: it shows up in the
display settings, can be arranged, mirrored or made primary, and is rendered
by the GPU that renders the session.

It uses mutter's `org.gnome.Mutter.ScreenCast.RecordVirtual` API, so it needs
GNOME on Wayland and nothing else: no kernel module, no EDID override, no
display plug.

## Build

```sh
meson setup build
meson compile -C build
meson install -C build   # optional, installs vmon
```

Build dependencies: a C compiler, meson, `libglib2.0-dev` and
`libpipewire-0.3-dev` (Debian/Ubuntu names).

## Usage

```sh
vmon WIDTHxHEIGHT@HZ[*] [WIDTHxHEIGHT@HZ[*] ...]
```

Each argument is a mode of the monitor. A `*` marks the preferred mode; without
one, the first mode is preferred. The other modes can be picked later in the
display settings or with `gdctl`.

```sh
vmon 2560x1600@120
vmon '1920x1080@60*' 1280x720@60
```

The monitor disappears when `vmon` exits. To keep one around without a
terminal, run it as a user service:

```sh
systemd-run --user --unit vmon vmon 2560x1600@120
systemctl --user stop vmon
```

`vmon` exits with status 0 on SIGINT or SIGTERM and 1 when the monitor goes
away for any other reason, so `Restart=on-failure` works as expected.

## How it works

`RecordVirtual` creates the monitor together with a PipeWire screen cast
stream of it, and mutter only keeps painting the monitor while that stream is
consumed. `vmon` is the consumer: it accepts whatever formats mutter offers
(DMA-BUF included, so frames never go through the CPU) and hands every buffer
straight back.

`vmon` also drives the stream. Left to its own clock, a virtual monitor stops
painting for clients that time their commits, such as Vulkan applications
presenting in FIFO mode. As the PipeWire driver, `vmon` makes mutter paint the
monitor on request instead, and turns each request into one cycle no faster
than the refresh rate of the current mode.

Tested with GNOME 50.
