#!/usr/bin/env python3
"""
Query all WSN nodes (from devices.yaml) for cluster report and print aggregated data.
Each node must be connected; send CLUSTER over serial and parse CLUSTER_REPORT_* block.

Usage:
  python get_cluster_data.py              # use devices.yaml in same dir
  python get_cluster_data.py --ports /dev/cu.usbmodem1 /dev/cu.usbmodem2
"""
from pathlib import Path
import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    print("Install pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(1)

try:
    import yaml
except ImportError:
    yaml = None

SCRIPT_DIR = Path(__file__).resolve().parent
BAUD = 115200
READ_TIMEOUT = 0.5
WRITE_TIMEOUT = 1.0
BOOT_WAIT_AFTER_OPEN = 5.5   # Opening serial often resets ESP32; wait for boot before sending CLUSTER
COLLECT_TIMEOUT = 3.0        # Read this long for CLUSTER_REPORT response

# Fallback ports if devices.yaml missing or no PyYAML (edit for your setup)
DEFAULT_PORTS = [
    "/dev/cu.usbmodem5ABA0603751",
    "/dev/cu.usbmodem5ABA0605221",
    "/dev/cu.usbmodem5ABA0607321",
]


def _find_devices_file():
    """Try script dir, then cwd, then cwd/ms_node."""
    for d in [SCRIPT_DIR, Path.cwd(), Path.cwd() / "ms_node"]:
        f = d / "devices.yaml"
        if f.exists():
            return f
    return None


def load_devices(ports_override=None):
    if ports_override:
        return [{"name": f"Node{i+1}", "port": p} for i, p in enumerate(ports_override)]
    dev_file = _find_devices_file()
    if dev_file and yaml:
        try:
            with open(dev_file) as f:
                data = yaml.safe_load(f) or {}
            out = data.get("devices", [])
            if out:
                return out
        except Exception:
            pass
    # No PyYAML or parse failed: parse devices.yaml by hand
    if dev_file:
        try:
            text = dev_file.read_text()
            devices = []
            for part in text.split("- name:")[1:]:  # skip first chunk (before first "- name:"
                lines = [ln.strip() for ln in part.splitlines() if ln.strip()]
                name = "Node?"
                if lines and not lines[0].startswith("port:"):
                    name = lines[0].strip('"')
                for line in lines:
                    if line.startswith("port:"):
                        port = line.split(":", 1)[1].strip().strip('"')
                        if port.startswith("/dev/") or "COM" in port:
                            devices.append({"name": name, "port": port})
                        break
            if devices:
                return devices
        except Exception:
            pass
    # Fallback: use default ports so script works without devices.yaml
    print("Using default ports (no devices.yaml or PyYAML). Pass --ports to override.", file=sys.stderr)
    return [{"name": f"Node{i+1}", "port": p} for i, p in enumerate(DEFAULT_PORTS)]


def query_cluster_report(port, baud=BAUD):
    """Open port, wait for boot, send CLUSTER\\n, collect output and parse one CLUSTER_REPORT block."""
    report = {}
    try:
        ser = serial.Serial(port, baud, timeout=READ_TIMEOUT, write_timeout=WRITE_TIMEOUT)
    except Exception as e:
        return {"_error": str(e), "_port": port}
    try:
        ser.reset_input_buffer()
        time.sleep(BOOT_WAIT_AFTER_OPEN)  # Let device finish boot after DTR reset
        ser.reset_input_buffer()
        ser.write(b"CLUSTER\n")
        time.sleep(0.2)
        deadline = time.monotonic() + COLLECT_TIMEOUT
        buf = ""
        in_block = False
        while time.monotonic() < deadline:
            line = ser.readline()
            if not line:
                time.sleep(0.05)
                continue
            try:
                s = line.decode("utf-8", errors="replace").strip()
            except Exception:
                continue
            buf += s + "\n"
            # Strip ESP-IDF log prefix e.g. "I (12345) main: "
            if ": " in s:
                s = s.split(": ", 1)[-1].strip()
            if "CLUSTER_REPORT_START" in s:
                in_block = True
                continue
            if "CLUSTER_REPORT_END" in s:
                break
            if in_block and "=" in s:
                key, _, val = s.partition("=")
                key = key.strip()
                val = val.strip()
                if key == "MEMBER_ID":
                    report.setdefault("MEMBER_IDS", []).append(val)
                elif key == "MEMBER_MAC":
                    report.setdefault("MEMBER_MACS", []).append(val)
                elif key == "MEMBER_SCORE":
                    report.setdefault("MEMBER_SCORES", []).append(val)
                else:
                    report[key] = val
        if not in_block and "CLUSTER_REPORT_START" in buf:
            report = {}
            in_block = False
            for line in buf.splitlines():
                if ": " in line:
                    line = line.split(": ", 1)[-1].strip()
                if "CLUSTER_REPORT_END" in line:
                    break
                if "CLUSTER_REPORT_START" in line:
                    in_block = True
                    continue
                if in_block and "=" in line:
                    key, _, val = line.partition("=")
                    key, val = key.strip(), val.strip()
                    if key == "MEMBER_ID":
                        report.setdefault("MEMBER_IDS", []).append(val)
                    elif key == "MEMBER_MAC":
                        report.setdefault("MEMBER_MACS", []).append(val)
                    elif key == "MEMBER_SCORE":
                        report.setdefault("MEMBER_SCORES", []).append(val)
                    else:
                        report[key] = val
    except Exception as e:
        report["_error"] = str(e)
    finally:
        ser.close()
    return report


def main():
    ap = argparse.ArgumentParser(description="Get cluster data from all nodes")
    ap.add_argument("--ports", nargs="+", help="Override serial ports")
    ap.add_argument("--baud", type=int, default=BAUD, help="Baud rate")
    args = ap.parse_args()
    devices = load_devices(args.ports)
    if not devices:
        print("No devices. Create devices.yaml in this dir or pass --ports /dev/cu.usbmodemXXX ...", file=sys.stderr)
        sys.exit(1)

    results = []
    for dev in devices:
        port = dev.get("port")
        name = dev.get("name", port)
        print(f"Querying {name} ({port})...", file=sys.stderr)
        r = query_cluster_report(port, args.baud)
        r["_name"] = name
        r["_port"] = port
        results.append(r)

    # Print table
    print("\n" + "=" * 80)
    print("CLUSTER DATA")
    print("=" * 80)
    ch_node = None
    for r in results:
        name = r.get("_name", "?")
        if r.get("_error"):
            print(f"\n{name} ({r.get('_port', '')}): ERROR - {r['_error']}")
            continue
        node_id = r.get("NODE_ID", "?")
        mac = r.get("MAC", "?")
        role = r.get("ROLE", "?")
        is_ch = r.get("IS_CH", "0") == "1"
        stellar = r.get("STELLAR_SCORE", "?")
        composite = r.get("COMPOSITE_SCORE", "?")
        battery = r.get("BATTERY", "?")
        trust = r.get("TRUST", "?")
        linkq = r.get("LINK_QUALITY", "?")
        current_ch = r.get("CURRENT_CH", "0")
        member_count = r.get("MEMBER_COUNT", "0")
        if is_ch:
            ch_node = (name, node_id, mac)
        print(f"\n--- {name} ---")
        print(f"  NODE_ID:      {node_id}")
        print(f"  MAC:         {mac}")
        print(f"  ROLE:        {role}")
        print(f"  IS_CH:       {is_ch}")
        print(f"  STELLAR:     {stellar}  COMPOSITE: {composite}")
        print(f"  BATTERY:     {battery}  TRUST: {trust}  LINK_Q: {linkq}")
        print(f"  CURRENT_CH:  {current_ch}")
        print(f"  MEMBER_CNT:  {member_count}")
        sensors_real = r.get("SENSORS_REAL", "")
        battery_real = r.get("BATTERY_REAL", "")
        if sensors_real != "" or battery_real != "":
            print(f"  SENSORS_REAL: {sensors_real}  BATTERY_REAL: {battery_real}")
        if r.get("MEMBER_IDS"):
            print(f"  MEMBERS:     {', '.join(r['MEMBER_IDS'])}")
            for i, mid in enumerate(r["MEMBER_IDS"]):
                mac_m = r.get("MEMBER_MACS", [])
                sc = r.get("MEMBER_SCORES", [])
                mac_s = mac_m[i] if i < len(mac_m) else "?"
                sc_s = sc[i] if i < len(sc) else "?"
                print(f"    node_{mid}  MAC={mac_s}  score={sc_s}")

    print("\n" + "=" * 80)
    print("SUMMARY")
    print("=" * 80)
    total = len([r for r in results if not r.get("_error")])
    print(f"Total nodes queried: {total}")
    if ch_node:
        print(f"Cluster head: {ch_node[0]}  (node_id={ch_node[1]}, MAC={ch_node[2]})")
    members = [r.get("_name") for r in results if not r.get("_error") and r.get("IS_CH") != "1"]
    if members:
        print(f"Members: {', '.join(members)}")
    print()


if __name__ == "__main__":
    main()
