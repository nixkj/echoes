#!/usr/bin/env python3
"""
Simple Firmware Update Server for ESP32 Echoes System

This script creates a basic HTTP server to host firmware files for OTA updates.

Usage:
    python3 firmware_server.py [port]

Default port: 8000

Directory structure:
    $HOME/firmware_server/
    ├── firmware/
    │   ├── version.txt
    │   └── bird_system.bin

Access URLs:
    http://<your-ip>:8000/firmware/version.txt
    http://<your-ip>:8000/firmware/bird_system.bin
"""

import http.server
import socketserver
import os
import sys
from datetime import datetime
from pathlib import Path

PORT = 8000 if len(sys.argv) < 2 else int(sys.argv[1])

# Use $HOME/firmware_server as the base directory
HOME = str(Path.home())
BASE_DIR = os.path.join(HOME, "firmware_server")
FIRMWARE_DIR = os.path.join(BASE_DIR, "firmware")

class FirmwareHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    """Custom request handler with logging and CORS support"""
    
    def do_GET(self):
        """Handle GET requests with logging"""
        # Add CORS headers for cross-origin requests
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        
        # Determine content type
        if self.path.endswith('.bin'):
            self.send_header('Content-Type', 'application/octet-stream')
        elif self.path.endswith('.txt'):
            self.send_header('Content-Type', 'text/plain')
        else:
            self.send_header('Content-Type', 'text/html')
        
        # Get file path
        file_path = self.translate_path(self.path)
        
        # Check if file exists
        if os.path.exists(file_path) and os.path.isfile(file_path):
            file_size = os.path.getsize(file_path)
            self.send_header('Content-Length', str(file_size))
            self.end_headers()
            
            # Send file content
            with open(file_path, 'rb') as f:
                self.wfile.write(f.read())
            
            # Log the download
            timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
            print(f"[{timestamp}] {self.client_address[0]} - Downloaded: {self.path} ({file_size} bytes)")
        else:
            # File not found
            self.send_error(404, "File not found")
            print(f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] {self.client_address[0]} - 404: {self.path}")
    
    def do_OPTIONS(self):
        """Handle OPTIONS requests for CORS preflight"""
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()
    
    def log_message(self, format, *args):
        """Override default logging to reduce verbosity"""
        # Only log errors
        if args[1].startswith('4') or args[1].startswith('5'):
            super().log_message(format, *args)

def ensure_firmware_directory():
    """Create firmware directory if it doesn't exist"""
    if not os.path.exists(FIRMWARE_DIR):
        os.makedirs(FIRMWARE_DIR, exist_ok=True)
        print(f"Created {FIRMWARE_DIR}/ directory")
        
        # Create sample version.txt
        version_file = os.path.join(FIRMWARE_DIR, "version.txt")
        with open(version_file, 'w') as f:
            f.write("1.0.0\n")
        print(f"Created sample {version_file}")
        
        # Create README
        readme_file = os.path.join(FIRMWARE_DIR, "README.txt")
        with open(readme_file, 'w') as f:
            f.write("""ESP32 Firmware Files
====================

Place your firmware files in this directory:

1. version.txt - Current firmware version (e.g., "1.0.1")
2. bird_system.bin - Compiled firmware binary

To update firmware:
1. Build your project: idf.py build
2. Copy binary: cp build/echoes.bin {FIRMWARE_DIR}/bird_system.bin
3. Update version: echo "1.0.1" > {FIRMWARE_DIR}/version.txt

ESP32 devices will check this server for updates.
""".format(FIRMWARE_DIR=FIRMWARE_DIR))
        print(f"Created {readme_file}")
    else:
        # Check if version.txt exists
        version_file = os.path.join(FIRMWARE_DIR, "version.txt")
        if not os.path.exists(version_file):
            print(f"WARNING: {version_file} not found!")
            print(f"Create it with: echo '1.0.0' > {version_file}")
        
        # Check if firmware binary exists
        binary_file = os.path.join(FIRMWARE_DIR, "bird_system.bin")
        if not os.path.exists(binary_file):
            print(f"WARNING: {binary_file} not found!")
            print("Copy it from your build: cp build/echoes.bin " + FIRMWARE_DIR + "/bird_system.bin")

def get_local_ip():
    """Get local IP address"""
    import socket
    try:
        # Create a socket and connect to an external address
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()
        return local_ip
    except:
        return "localhost"

def main():
    """Start the firmware server"""
    print("=" * 60)
    print("Echoes of the Machine - Firmware Update Server")
    print("=" * 60)
    
    # Ensure firmware directory exists
    ensure_firmware_directory()
    
    # Get local IP
    local_ip = get_local_ip()
    
    # Change to base directory so server serves from there
    os.chdir(BASE_DIR)
    
    # Start server
    try:
        with socketserver.TCPServer(("", PORT), FirmwareHTTPRequestHandler) as httpd:
            print(f"\nServer running on port {PORT}")
            print(f"Base directory: {BASE_DIR}")
            print(f"\nAccess URLs:")
            print(f"  Local:   http://localhost:{PORT}/firmware/")
            print(f"  Network: http://{local_ip}:{PORT}/firmware/")
            print(f"\nFirmware files:")
            print(f"  Version: http://{local_ip}:{PORT}/firmware/version.txt")
            print(f"  Binary:  http://{local_ip}:{PORT}/firmware/bird_system.bin")
            print(f"\nUpdate main/ota.h with these URLs:")
            print(f"  #define OTA_URL         \"http://{local_ip}:{PORT}/firmware/bird_system.bin\"")
            print(f"  #define VERSION_URL     \"http://{local_ip}:{PORT}/firmware/version.txt\"")
            print(f"\nPress Ctrl+C to stop the server")
            print("=" * 60)
            print("\nWaiting for requests...\n")
            
            # Serve forever
            httpd.serve_forever()
            
    except KeyboardInterrupt:
        print("\n\nShutting down server...")
        print("Server stopped.")
    except OSError as e:
        if e.errno == 48 or e.errno == 98:  # Address already in use
            print(f"\nERROR: Port {PORT} is already in use!")
            print(f"Try a different port: python3 firmware_server.py 8001")
        else:
            print(f"\nERROR: {e}")
    except Exception as e:
        print(f"\nERROR: {e}")

if __name__ == "__main__":
    main()
