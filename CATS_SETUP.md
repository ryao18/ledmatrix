# Cats display setup on a Raspberry Pi Zero W

This guide sets up `cats` on a single 64×32 panel, with 21×21 portraits of Ocean and Fungus, the clock and date, and the daily fact ticker. You can run it from an SSH terminal or have Codex on the Mac run the same commands over SSH.

## 1. Connect and inspect the existing setup

From the Mac, use your Pi login and its Tailscale hostname or IP address.

```sh
ssh your-user@your-pi
```

For a new Pi, use [Raspberry Pi OS Lite (32-bit)](https://www.raspberrypi.com/software/operating-systems/) and [configure Wi-Fi and SSH in Raspberry Pi Imager](https://www.raspberrypi.com/documentation/computers/getting-started.html). Use the [adapter and wiring guide](./adapter/README.md) for the panel. The remaining commands run on the Pi and assume the panel is connected and powered.

If `cats-display` is already installed, check its current setup before changing anything. Use the actual checkout path if yours differs.

```sh
systemctl cat cats-display.service --no-pager
systemctl show cats-display.service \
  -p ActiveState -p UnitFileState -p FragmentPath -p DropInPaths \
  -p User -p ExecStart -p WorkingDirectory
sudo journalctl -u cats-display.service -n 30 --no-pager
git -C ~/rpi-rgb-led-matrix status --short --branch
ls -l /opt/cats-display/fonts
free -h
uptime
```

Record which binary and photos `ExecStart` uses, whether it launches from the checkout or a separate install, and any panel flags set by the unit or its drop-ins. The log, available memory, and load give you a baseline while the display is running.

## 2. Build `cats` and install the fonts

Install the build dependencies on Raspberry Pi OS.

```sh
sudo apt update
sudo apt install build-essential ca-certificates git libgraphicsmagick++-dev \
  libwebp-dev libcurl4-openssl-dev libjsoncpp-dev
```

For a fresh checkout, run the following. If the repo is private, use a GitHub remote the Pi can authenticate to.

```sh
git clone https://github.com/ryao18/ledmatrix.git ~/rpi-rgb-led-matrix
```

If updating an existing service, stop it before pulling and rebuilding. Leave it stopped until the updated binary and assets are in place in step 3.

```sh
sudo systemctl stop cats-display.service
```

For an existing checkout on `main`, pull the latest version. Use your actual checkout path throughout the guide if it differs.

```sh
cd ~/rpi-rgb-led-matrix
git pull --ff-only
```

Build only `cats` with one job to keep memory use down on the Zero W. The hardware build reads the fonts from `/opt/cats-display/fonts`.

```sh
cd ~/rpi-rgb-led-matrix
make -j1 -C examples-api-use cats
sudo install -d -m 0755 /opt/cats-display/fonts
sudo install -m 0644 fonts/5x7.bdf fonts/6x13.bdf fonts/5x8.bdf /opt/cats-display/fonts/
```

## 3. Start the service

### Fresh install

Install the binary and both pictures under `/opt/cats-display`.

```sh
cd ~/rpi-rgb-led-matrix
sudo install -d -m 0755 /opt/cats-display/img
sudo install -m 0755 examples-api-use/cats /opt/cats-display/cats
sudo install -m 0644 img/ocean_portrait_blue_square.png img/fungus_portrait_square.png /opt/cats-display/img/
```

Create the unit below. Add any flags required by your panel or adapter to `ExecStart` before the image paths. The service starts with GPIO access and the driver then drops privileges to `daemon`.

```sh
sudo tee /etc/systemd/system/cats-display.service >/dev/null <<'UNIT'
[Unit]
Description=Cats LED matrix display

[Service]
Type=simple
WorkingDirectory=/opt/cats-display
ExecStart=/opt/cats-display/cats --led-rows=32 --led-cols=64 /opt/cats-display/img/ocean_portrait_blue_square.png /opt/cats-display/img/fungus_portrait_square.png
Restart=on-failure
RestartSec=5s

[Install]
WantedBy=multi-user.target
UNIT

sudo systemctl daemon-reload
sudo systemctl enable --now cats-display.service
```

### Existing service

Keep the hardware flags from the current unit. If it runs `cats` from the checkout, the build in step 2 updates its binary. If it runs a separate copy, install the rebuilt binary in its deployment directory. Copy the new portraits into the image directory used by the service, using the image copy command above if that directory is `/opt/cats-display/img`.

Update the two picture paths in `ExecStart` to use `ocean_portrait_blue_square.png` and `fungus_portrait_square.png`. Relative paths resolve from `WorkingDirectory`. Make sure `daemon` can read both pictures and traverse their directories. If `systemctl cat` shows a drop-in that sets `ExecStart`, edit that drop-in.

```sh
sudo systemctl edit --full cats-display.service
```

If the unit already points to the rebuilt or redeployed binary and both new portraits, skip the edit. Reload after changing a unit file, then start the service.

```sh
sudo systemctl daemon-reload
sudo systemctl start cats-display.service
```

## 4. Verify the display

```sh
systemctl status cats-display.service --no-pager
sudo journalctl -u cats-display.service -n 30 --no-pager
timedatectl
```

Check that both portraits rotate, the clock and date match local time, and the fact appears once fetched or loaded from the cache. The display runs at 100% brightness from 8:00 a.m. and dims to 10% at 11:30 p.m. using the Pi’s local time. For a Pi in Pittsburgh, set the time zone with `sudo timedatectl set-timezone America/New_York` if needed.

If the service reports missing images or fonts, check the paths from steps 2 and 3. If it reports a conflict with `snd_bcm2835`, turn off onboard audio with `dtparam=audio=off` in `/boot/firmware/config.txt` (`/boot/config.txt` before Bookworm) and reboot. For other GPIO and flicker issues, see the [matrix troubleshooting notes](./README.md#troubleshooting).

## 5. Optional 100 Hz refresh test

Stop the service so only the foreground test drives the panel. Match the working directory, binary, image paths, and panel flags from your service, then use the refresh flags below. These examples use the `/opt/cats-display` install. Each test runs about a minute and `timeout` normally returns status `124`.

Measure the uncapped default first.

```sh
cd /opt/cats-display
sudo systemctl stop cats-display.service
sudo timeout --signal=INT --kill-after=5s 60s ./cats \
  --led-rows=32 --led-cols=64 --led-show-refresh --led-limit-refresh=0 \
  img/ocean_portrait_blue_square.png img/fungus_portrait_square.png
```

If the baseline stays comfortably above 100 Hz, test the cap with busy waiting disabled. Compare CPU usage and SSH responsiveness during both runs, and check the panel for flicker when you can see it.

```sh
sudo timeout --signal=INT --kill-after=5s 60s ./cats \
  --led-rows=32 --led-cols=64 --led-show-refresh --led-limit-refresh=100 \
  --led-no-busy-waiting img/ocean_portrait_blue_square.png img/fungus_portrait_square.png
```

Start the service again with its configured settings.

```sh
sudo systemctl start cats-display.service
```

To keep the cap, add `--led-limit-refresh=100 --led-no-busy-waiting` to the service’s `ExecStart` before the image paths. Apply the change with `sudo systemctl daemon-reload` and `sudo systemctl restart cats-display.service`, then check its status and logs again. Remove those two flags and repeat the reload and restart to return to the uncapped default.
