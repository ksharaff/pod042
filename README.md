# POD-042

A NieR:Automata-inspired system diagnostics tool for the Linux terminal, written in C++20.

You boot it like an android's internal operating system. Behind the theme it is a real, live monitor: it reads CPU, memory, disk, network, processes and connections straight from `/proc` and the kernel, and reports what changes.

<img width="1890" height="902" alt="image" src="https://github.com/user-attachments/assets/de31dabe-509b-484d-8e69-cc3b9f139bff" />


## What it does

- **Live system status**: CPU, memory, disk and network speed, with sparklines of the last 16 seconds.
- **System details**: uptime, per-core CPU bars, and mounted filesystems.
- **Chip system**: features are "OS chips" that cost slots from a fixed budget of 4. A chip that is not installed does not run, so the tool only does the work you ask for. Loadouts are saved between runs, and three presets (DIAGNOSTIC, EXPLORATION, SECURITY) switch the whole set at once.
- **Network connections**: IPv4 and IPv6 TCP sockets with their state and owning process. A listener open on all interfaces is highlighted.
- **Process watch**: the busiest processes, with a log warning when one stays above a CPU threshold.
- **Event feed**: new listeners, new inbound and outbound connections, and programs seen for the first time, written to the log. Known programs are remembered between runs.
- **COMMS**: send text messages to another pod over TCP (see the security note below).
- **Boot sequence**, **config file**, and a **`--json`** snapshot for scripts.

## Screenshots

> 📷 **PLACEHOLDER: boot sequence**
> File: `docs/screenshots/boot.png`
> Show: the typewriter boot screen part-way through, with the progress bar and your pod name on the first line.

> 📷 **PLACEHOLDER: system details**
> File: `docs/screenshots/details.png`
> Show: the SYSTEM DETAILS screen with the per-core bars and the mounted filesystems.

> 📷 **PLACEHOLDER: network connections**
> File: `docs/screenshots/connections.png`
> Show: the NETWORK CONNECTIONS table with the OWNER column filled in and one amber listener row. Check the addresses and process names before publishing (see the notes at the end).

> 📷 **PLACEHOLDER: chip configuration**
> File: `docs/screenshots/chips.png`
> Show: the CHIP CONFIGURATION screen with a few chips installed, the slot gauge, and the preset line.

> 📷 **PLACEHOLDER: comms**
> File: `docs/screenshots/comms.png`
> Show: the COMMS screen with a short conversation between two pods, the link line, and the message box at the bottom.

> 📷 **PLACEHOLDER: event log**
> File: `docs/screenshots/logs.png`
> Show: the SYSTEM LOGS screen with a few feed entries (a new listener, a new connection, a warning in amber).

> 🎞 **OPTIONAL PLACEHOLDER: demo GIF**
> File: `docs/demo.gif`
> Show: boot sequence, then switching through the screens and installing a chip (about 15 seconds).

## Requirements

- Linux (developed on Ubuntu under WSL2)
- A C++20 compiler (GCC 11 or newer) and CMake 3.16 or newer
- Internet access the first time CMake configures the project: it downloads [FTXUI](https://github.com/ArthurSonzogni/FTXUI) and [nlohmann/json](https://github.com/nlohmann/json)

## Build and run

```bash
cmake -B build
cmake --build build -j
./build/pod042
```

The first run asks for a name for your pod and saves it.

## Controls

| Key | Action |
|---|---|
| Up / Down | choose a screen or a chip |
| Left / Right | move focus between the menu and the screen |
| Enter | install or remove the selected chip; send a message in COMMS |
| 1 - 3 | load a preset (on the CHIP CONFIGURATION screen) |
| Esc | leave the COMMS message box |
| Any key | skip the boot sequence |
| Q | quit |

## Command-line options

| Option | What it does |
|---|---|
| `--json` | print one snapshot as JSON and exit |
| `--rename` | ask for the pod name again |
| `--init-config` | write a `config.json` with every setting at its default |
| `--check-config` | show how `config.json` was read, and any problems |
| `--forget-programs` | clear the saved list of known programs |

## Configuration

Settings live in an optional `config.json` in the folder you run the program from. Anything missing keeps its default, and an invalid value is ignored and reported in the log. Run `./build/pod042 --init-config` to create one.

```json
{
  "gauge_warn_percent": 80,
  "cpu_alert_percent": 80,
  "mem_alert_percent": 90,
  "process_alert_percent": 80,
  "heartbeat_seconds": 60,
  "expected_ports": [22],
  "comms_port": 4242,
  "comms_bind": "127.0.0.1",
  "comms_connect": [],
  "comms_allow": []
}
```

`expected_ports` lists ports that are supposed to listen on all interfaces. Any other listener there is flagged.

## COMMS security note

COMMS (phase 1) is plain TCP: it is **not encrypted and not authenticated**. Anyone on the network can read the traffic, and a sender's name is only a claim. Use it on trusted local networks only. It never opens a listening port unless `comms_allow` is set, only accepts connections from the addresses listed there, and binds to `127.0.0.1` by default. Every incoming message is cleaned and size-limited before it is shown.

To try it on one machine, give two pods separate folders and configs: one with `"comms_allow": ["127.0.0.1"]`, the other with `"comms_connect": ["127.0.0.1:4242"]`, then install the COMMS chip in both.

## Files it creates

In the folder you run it from: `pod042.log`, `loadout.json`, `profile.json`, `known_programs.json`, and `config.json` if you create one.

## Limitations

- Linux only for now. It reads `/proc`, so under WSL it shows the Linux VM, not the Windows host.
- IPv4 and IPv6 TCP only (no UDP).
- Seeing the owner of every socket needs root; as a normal user you see your own processes.
- Settings are read once at startup.
- Planned: encrypted and authenticated COMMS, journal and failed-login view, a Windows port.
