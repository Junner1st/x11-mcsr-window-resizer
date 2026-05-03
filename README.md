# X11 MCSR Windows Resizer

Low-latency X11 window resizer for switching a target window between fixed
`thin`, `wide`, and `full` geometries.


This is inspired by [linux-mcsr-resizer](https://github.com/nafetss/linux-mcsr-resizer) from [nafetss](https://github.com/nafetss). But instead of a shell script, this use systemctl service to improve performance and try to fix some bugs.


## Configure

Edit `mc-resizer.conf`.

Common settings: monitor geometry, thin/wide size, hotkeys, `backend`, and
`refresh`.

## Install As User Service

```sh
make install-service
systemctl --user import-environment DISPLAY XAUTHORITY XDG_RUNTIME_DIR
systemctl --user enable --now mc-resizerd.service
```

If the daemon cannot see the desktop, verify the imported environment:

```sh
systemctl --user show-environment | grep -E 'DISPLAY|XAUTHORITY|XDG_RUNTIME_DIR'
```

## Run Manually


### Build

```sh
make
```

### Excution

```sh
./mc-resizerd --config ./mc-resizer.conf --verbose
```

In another terminal:

```sh
./mc-resizerctl status
./mc-resizerctl thin
./mc-resizerctl wide
./mc-resizerctl full
./mc-resizerctl cycle
```

## Commands

- `thin`
- `wide`
- `full`
- `cycle`
- `status`
- `rescan`
- `quit`

## Behavior

`mc-resizerd` finds a matching X11 window, caches its id/geometry, and resizes
it from hotkeys or socket commands.

It does not read process memory, game state, logs, screen pixels, or generate
keyboard/mouse input.
