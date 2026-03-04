#!/usr/bin/env python3
"""
WSN Onboarding Node - ROS2 + Flask server for UAV data collection.

Changes from original:
- /data endpoint now deduplicates by (id, seq) and sorts output per node
- Output files are clean: one reading per (node_id, seq), ordered by seq
- FIXED: Flask now runs threaded to prevent TCP stalls during heavy processing
- FIXED: WiFi power management disabled on AP interface to prevent beacon drops
"""
import time
import subprocess
import threading
import csv
import hmac
import hashlib
import json
import re

import rclpy
from rclpy.node import Node
from rpi_rf import RFDevice
from std_srvs.srv import Trigger
from flask import Flask, request, jsonify

RF_CODE = 22
ALLOWED_MACS = ["10:20:BA:4C:59:8C", "10:20:BA:4D:EB:1C", "10:20:BA:4E:26:80"]
SECRET_KEY = b"pi_secret_key_12345"
NODE_REGISTRY = {}  # node_id -> info
WIFI_IFACE = "wlan1"
ACTIVE_SESSION = {}  # session_id -> node_id

def verify_token(node_id, mac, token, metadata=""):
    if mac.upper() not in ALLOWED_MACS:
        return False
    payload = f"{node_id}|{metadata}"
    expected_token = hmac.new(SECRET_KEY, payload.encode(), hashlib.sha256).hexdigest()
    return hmac.compare_digest(expected_token, token)

class WsnOnboardingNode(Node):
    def __init__(self):
        super().__init__('wsn_onboarding_node')

        self.tx_pin = 17
        self.rfdevice = RFDevice(self.tx_pin)
        self.rfdevice.enable_tx()  # enable TX

        self.rf_code = RF_CODE
    
        self.wsn_trigger = False
        self.wsn_node_connected = False
        self.wsn_onboarding_status = False
        self.stop_rf_event = threading.Event()
        self.onboard_count = 0  # tracks how many onboarding cycles completed
        self.current_session_id = None  # session for current onboarding cycle
        self._cycle_id = 0  # monotonic cycle counter to guard stale cleanup
        self._ack_received = threading.Event()  # signalled when /ack arrives

        self.status_srv = self.create_service(
            Trigger,
            '/wsn_onboarding/status',
            self.wsn_status_callback
        )

        self.trigger_srv = self.create_service(
            Trigger,
            '/wsn_onboarding/trigger',
            self.wsn_trigger_callback
        )

        self.app = Flask(__name__)
        self.setup_flask()

    def send_rf(self):
        """RF transmission loop - runs in separate thread"""
        TARGET_MACS = [m.upper() for m in ALLOWED_MACS]
        timeout = 60  # seconds
        interval = 10  # seconds between RF sends
        start_time = time.time()

        self.get_logger().info("Starting RF wake-up sequence...")

        while not self.stop_rf_event.is_set():
            elapsed = time.time() - start_time
            self.get_logger().info(f"Sending RF code: {self.rf_code} (elapsed: {elapsed:.1f}s)")
            repeats = 8
            for i in range(repeats):
                self.rfdevice.tx_code(self.rf_code, 1, 350, 24) 
                time.sleep(0.05)
            try:
                arp_output = subprocess.check_output(
                    "arp -n | awk 'NR>1 { print $3 }'", shell=True
                ).decode().splitlines()
                arp_macs = [x.upper() for x in arp_output if x]
                
                joined = [mac for mac in TARGET_MACS if mac in arp_macs]
                if joined:
                    self.get_logger().info(f"Node(s) joined Wi-Fi: {joined}")
                    self.wsn_node_connected = True
                    self.get_logger().info("Node joined — stopping RF transmission")
                    break
            except Exception as e:
                self.get_logger().warn(f"ARP check failed: {e}")

            if elapsed > timeout:
                self.get_logger().warn("RF wake-up timeout, stopping transmission")
                break
            
            self.stop_rf_event.wait(interval)

        self.get_logger().info("RF transmission loop ended")

        if self.wsn_node_connected:
            # Node joined Wi-Fi — wait for /ack to arrive before tearing down.
            watchdog = 120  # seconds
            self.get_logger().info(f"Node connected — waiting up to {watchdog}s for /ack...")
            ack_arrived = self._ack_received.wait(timeout=watchdog)
            if ack_arrived:
                self.get_logger().info("ACK confirmed — proceeding to cleanup")
            else:
                self.get_logger().warn("Onboard watchdog expired — node connected but /ack never arrived")
        else:
            self.get_logger().warn("No node connected — resetting for next trigger")

        # RF thread owns the cleanup — stop hotspot and reset trigger
        self._do_cleanup()

    def setup_flask(self):
        @self.app.route('/onboard', methods=['POST'])
        def onboard():
            data = request.json
            node_id = data.get("node_id")
            mac = data.get("mac")
            token = data.get("token")
            metadata = data.get("metadata", "")

            self.get_logger().info(f"Onboard request from Node={node_id}, MAC={mac}")

            if node_id in NODE_REGISTRY:
                # Node already onboarded - create new session
                self.get_logger().info(f"Node {node_id} already onboarded, creating new session")
            
            if not verify_token(node_id, mac, token, metadata):
                self.get_logger().warn(f"Rejected Node={node_id}, MAC={mac} (token verification failed)")
                return jsonify({"status": "REJECTED"}), 403
            
            session_id = hashlib.sha256(
                f"{node_id}{time.time()}".encode()
            ).hexdigest()[:16]

            NODE_REGISTRY[node_id] = {
                "mac": mac, 
                "metadata": metadata
            }

            ACTIVE_SESSION[session_id] = node_id
            self.current_session_id = session_id

            self.get_logger().info(f"Accepted Node={node_id}, MAC={mac}, Session={session_id}")
            with open("/home/punky/ros2_ws/data/onboarded_nodes.csv", "a", newline="") as f:
                writer = csv.writer(f)
                writer.writerow([node_id, mac, metadata, time.time()])

            return jsonify({
                "status": "ACCEPTED",
                "session_id": session_id, 
                "sample_rate": 5
            })
        
        @self.app.route('/data', methods=['POST'])
        def data_upload():
            session_id = request.headers.get("X-Session-ID")
            data_status = request.headers.get("X-Data-Status", "")
            
            if not session_id or session_id not in ACTIVE_SESSION:
                self.get_logger().warn(f"Data upload with invalid session: {session_id}")
                return jsonify({"status": "INVALID_SESSION"}), 403
            
            node_id = ACTIVE_SESSION[session_id]
            
            # Handle empty storage notification
            if data_status == "empty":
                self.get_logger().info(f"Node {node_id} reports empty storage — no data to upload")
                return jsonify({"status": "DATA_OK", "note": "empty acknowledged"}), 200
            
            data_content = request.data.decode('utf-8', errors='replace')
            
            # Split concatenated JSON objects into individual lines
            # The data arrives as back-to-back JSON: {...}{...}{...}
            json_objects = re.findall(r'\{[^{}]*\}', data_content)
            
            # -----------------------------------------------------------
            # DEDUP + SORT: Parse each JSON, deduplicate by (id, seq),
            # then sort by (id, seq) for clean ordered output
            # -----------------------------------------------------------
            seen = {}  # key: (id, seq) -> parsed dict
            parse_errors = 0
            
            for obj_str in json_objects:
                try:
                    obj = json.loads(obj_str)
                    node_data_id = obj.get("id", 0)
                    seq = obj.get("seq", 0)
                    key = (node_data_id, seq)
                    
                    if key not in seen:
                        seen[key] = obj
                    # else: duplicate, skip
                except json.JSONDecodeError:
                    parse_errors += 1
            
            # Sort by (id, seq) for clean sequential output
            sorted_entries = sorted(seen.values(), key=lambda x: (x.get("id", 0), x.get("seq", 0)))
            
            # Save deduplicated + sorted data
            filename = f"/home/punky/ros2_ws/data/data_{node_id}_ch{self.onboard_count}_{int(time.time())}.log"
            with open(filename, 'w') as f:
                for entry in sorted_entries:
                    f.write(json.dumps(entry, separators=(',', ':')) + '\n')
            
            total_raw = len(json_objects)
            total_deduped = len(sorted_entries)
            dupes_removed = total_raw - total_deduped
            
            self.get_logger().info(
                f"Received {len(data_content)} bytes from {node_id}: "
                f"{total_raw} records → {total_deduped} unique "
                f"({dupes_removed} duplicates removed, {parse_errors} parse errors), "
                f"saved to {filename}"
            )
            
            return jsonify({
                "status": "DATA_OK",
                "records_received": total_raw,
                "records_stored": total_deduped,
                "duplicates_removed": dupes_removed
            }), 200
        
        @self.app.route('/ack', methods=['POST'])
        def ack():
            data = request.json or {}
            session_id = data.get("session_id")

            if session_id not in ACTIVE_SESSION:
                self.get_logger().warn(f"ACK with invalid session: {session_id}")
                return jsonify({"status": "INVALID_SESSION"}), 403
            
            node_id = ACTIVE_SESSION.pop(session_id)
            self.wsn_onboarding_status = True
            self.onboard_count += 1
            self.current_session_id = None

            self.get_logger().info(f"ACK received from {node_id}, onboarding #{self.onboard_count} complete!")

            # Signal the RF/watchdog thread that /ack arrived;
            # that thread will handle hotspot teardown + state reset.
            self._ack_received.set()

            return jsonify({"status": "ACK_OK"}), 200

        # FIX: Enable threaded=True so Flask can handle concurrent requests
        # and doesn't stall TCP while processing heavy /data payloads
        threading.Thread(
            target=self.app.run,
            kwargs={"host": "0.0.0.0", "port": 8080, "debug": False, "threaded": True},
            daemon=True
        ).start()

    def wsn_trigger_callback(self, request, response):
        if not self.wsn_trigger:
            self.wsn_trigger = True
            self.wsn_onboarding_status = False
            self.data_onboarding()
            response.success = True
            response.message = f"Trigger successful (onboard cycle #{self.onboard_count + 1})"
        else:
            response.success = False
            response.message = "Trigger already in progress"
        return response

    def wsn_status_callback(self, request, response):
        if self.wsn_onboarding_status:
            response.success = True
            response.message = "ONBOARDING_DONE"
        else:
            response.success = False
            response.message = "ONBOARDING_PENDING"
        return response

    def data_onboarding(self):
        self.get_logger().info("Starting WSN data onboarding...")
        
        # Reset state for this new cycle
        self.wsn_node_connected = False
        self.wsn_onboarding_status = False
        self.stop_rf_event.clear()
        self._ack_received.clear()
        self._cycle_id += 1

        # Start Wi-Fi hotspot first
        try:
            result = subprocess.run([
                "sudo", "-n",
                "nmcli", "device", "wifi", "hotspot",
                "ifname", WIFI_IFACE,
                "ssid", "WSN_AP",
                "password", "raspberry"
            ], check=True, capture_output=True, text=True, timeout=10)
            self.get_logger().info(f"Wi-Fi AP started on {WIFI_IFACE} (SSID: WSN_AP)")
        except subprocess.TimeoutExpired:
            self.get_logger().error("Hotspot creation timed out")
            return
        except subprocess.CalledProcessError as e:
            self.get_logger().error(f"Failed to start Wi-Fi AP. Error: {e.stderr}, Output: {e.stdout}")
            return

        # Give hotspot time to fully initialize
        self.get_logger().info("Waiting for hotspot to stabilize...")
        time.sleep(3)

        # FIX: Disable WiFi power management on the AP interface to prevent
        # beacon drops that cause ESP32 bcn_timeout and connection loss
        try:
            subprocess.run(
                ["sudo", "-n", "iwconfig", WIFI_IFACE, "power", "off"],
                capture_output=True, timeout=5
            )
            self.get_logger().info(f"Disabled WiFi power management on {WIFI_IFACE}")
        except Exception as e:
            self.get_logger().warn(f"Failed to disable WiFi power management: {e}")

        # Start RF transmission in separate thread
        self.get_logger().info("Starting RF transmission thread...")
        rf_thread = threading.Thread(target=self.send_rf, daemon=True)
        rf_thread.start()
        
        self.get_logger().info("Onboarding sequence initiated - waiting for WSN nodes...")

    def stop_hotspot(self):
        self.get_logger().info("Waiting 5s before stopping hotspot...")
        time.sleep(5)
        
        try:
            subprocess.run(
                ["sudo", "-n", "nmcli", "connection", "down", "Hotspot"],
                capture_output=True,
                timeout=10
            )

            subprocess.run(
                ["sudo", "-n", "nmcli", "device", "disconnect", WIFI_IFACE],
                capture_output=True,
                timeout=10
            )

            self.get_logger().info("Wi-Fi hotspot stopped")
        except Exception as e:
            self.get_logger().error(f"Failed to stop hotspot: {e}")

    def _do_cleanup(self):
        """Stop hotspot and reset state so the next trigger can start a new cycle.
        Called only from the RF/watchdog thread — single owner, no races."""
        self.stop_hotspot()
        
        # Reset trigger flag so a new trigger can be accepted
        self.wsn_trigger = False
        self.wsn_node_connected = False
        self.get_logger().info("Onboarding cycle finished — ready for next trigger")
    
    def destroy_node(self):
        self.stop_rf_event.set()
        self.rfdevice.cleanup()
        super().destroy_node()


def main(args=None):
    rclpy.init(args=args)
    node = WsnOnboardingNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
