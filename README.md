# mutter-virtual-monitor

Command-line tool that adds a virtual monitor to a GNOME Wayland session. The monitor behaves like a real one: it shows up in the display settings, can be arranged, mirrored or made primary, and is rendered by the GPU that renders the session. It lasts as long as `vmon` runs.

## Build

```sh
meson setup build
meson compile -C build
```

Needs a C compiler, meson, and the GLib and PipeWire development files (`libglib2.0-dev`, `libpipewire-0.3-dev` on Debian and Ubuntu).

## Usage

```sh
vmon WIDTHxHEIGHT@HZ[*] [WIDTHxHEIGHT@HZ[*] ...]
```

Each argument is a mode of the monitor; `*` marks the preferred one, otherwise the first is preferred. Other modes can be picked later in the display settings or with `gdctl`.

```sh
vmon 2560x1600@120
systemd-run --user --unit vmon vmon '1920x1080@60*' 1280x720@60
```

`vmon` exits with status 0 on SIGINT or SIGTERM and 1 when the monitor goes away for any other reason.

## Compatibility

Tested on GNOME 50. `vmon` creates the monitor through mutter's `org.gnome.Mutter.ScreenCast.RecordVirtual` and reads its PipeWire stream as DMA-BUF.

`vmon` also sets the pace of that stream. Without it, a virtual monitor stalls applications that schedule their frames, such as Vulkan games with V-Sync on. With it, they run at the monitor's refresh rate.
