#!/usr/bin/env python3
"""
Test script for Echoes Startup Report Server

Sends simulated startup reports to test the server.
"""

import requests
import json
import time
import random
import argparse
from datetime import datetime


def generate_mac():
    """Generate a random MAC address"""
    return ":".join([f"{random.randint(0, 255):02X}" for _ in range(6)])


def send_test_report(url, mac=None, has_error=False):
    """Send a test startup report"""
    
    if mac is None:
        mac = generate_mac()
    
    data = {
        "mac": mac,
        "node_type": "echoes-v1",
        "avg_light": round(random.uniform(0, 500), 2) if not has_error else -1.0,
        "light_samples": random.randint(40, 60) if not has_error else 0,
        "sleep_duration_ms": random.randint(0, 5000),
        "has_errors": has_error,
        "error_message": "Test error: Light sensor timeout" if has_error else ""
    }
    
    print(f"\n{'='*70}")
    print(f"Sending test report at {datetime.now().strftime('%H:%M:%S')}")
    print(f"{'='*70}")
    print(json.dumps(data, indent=2))
    
    try:
        response = requests.post(
            url,
            json=data,
            headers={"Content-Type": "application/json"},
            timeout=5
        )
        
        print(f"\nResponse Status: {response.status_code}")
        
        if response.status_code == 200:
            print("✓ Success!")
            try:
                response_data = response.json()
                print(f"Server response: {json.dumps(response_data, indent=2)}")
            except:
                print(f"Server response: {response.text}")
        else:
            print(f"✗ Error: {response.text}")
            
    except requests.exceptions.ConnectionError:
        print("✗ Connection Error: Could not connect to server")
        print(f"   Make sure the server is running at {url}")
    except requests.exceptions.Timeout:
        print("✗ Timeout: Server did not respond in time")
    except Exception as e:
        print(f"✗ Error: {e}")


def main():
    parser = argparse.ArgumentParser(
        description='Test the Echoes Startup Report Server'
    )
    
    parser.add_argument(
        '--url',
        default='http://localhost:8000/startup',
        help='Server URL (default: http://localhost:8000/startup)'
    )
    
    parser.add_argument(
        '--count',
        type=int,
        default=1,
        help='Number of test reports to send (default: 1)'
    )
    
    parser.add_argument(
        '--interval',
        type=float,
        default=1.0,
        help='Interval between reports in seconds (default: 1.0)'
    )
    
    parser.add_argument(
        '--error-rate',
        type=float,
        default=0.0,
        help='Probability of error reports (0.0-1.0, default: 0.0)'
    )
    
    parser.add_argument(
        '--mac',
        help='Use a specific MAC address (default: random)'
    )
    
    args = parser.parse_args()
    
    print(f"Testing server at: {args.url}")
    print(f"Sending {args.count} report(s) with {args.interval}s interval")
    if args.error_rate > 0:
        print(f"Error probability: {args.error_rate * 100}%")
    print()
    
    for i in range(args.count):
        has_error = random.random() < args.error_rate
        send_test_report(args.url, args.mac, has_error)
        
        if i < args.count - 1:  # Don't sleep after last report
            time.sleep(args.interval)
    
    print(f"\n{'='*70}")
    print(f"Test complete. Sent {args.count} report(s)")
    print(f"{'='*70}\n")


if __name__ == '__main__':
    main()
