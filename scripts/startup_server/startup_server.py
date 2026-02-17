#!/usr/bin/env python3
"""
Echoes of the Machine - Startup Report Server

Simple HTTP server that receives startup reports from ESP32 devices
and logs them to a rotating log file.

Usage:
    python3 startup_server.py [--port PORT] [--logdir LOGDIR]
"""

import json
import logging
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime
from pathlib import Path
import argparse
import sys
from logging.handlers import RotatingFileHandler


class StartupReportHandler(BaseHTTPRequestHandler):
    """HTTP request handler for startup reports"""
    
    def do_POST(self):
        """Handle POST requests to /startup endpoint"""
        
        # Only accept POST to /startup
        if self.path != '/startup':
            self.send_error(404, "Not Found")
            return
        
        # Get content length
        content_length = int(self.headers.get('Content-Length', 0))
        
        if content_length == 0:
            self.send_error(400, "Empty request body")
            return
        
        # Read POST data
        post_data = self.rfile.read(content_length)
        
        try:
            # Parse JSON
            data = json.loads(post_data.decode('utf-8'))
            
            # Extract fields
            mac = data.get('mac', 'UNKNOWN')
            node_type = data.get('node_type', 'UNKNOWN')
            avg_light = data.get('avg_light', -1.0)
            light_samples = data.get('light_samples', 0)
            sleep_duration = data.get('sleep_duration_ms', 0)
            has_errors = data.get('has_errors', False)
            error_message = data.get('error_message', '')
            
            # Get client IP
            client_ip = self.client_address[0]
            
            # Log the report
            timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
            
            log_message = (
                f"[{timestamp}] Startup Report | "
                f"MAC: {mac} | "
                f"Type: {node_type} | "
                f"IP: {client_ip} | "
                f"Light: {avg_light:.2f} lux ({light_samples} samples) | "
                f"Sleep: {sleep_duration}ms | "
                f"Errors: {'YES' if has_errors else 'NO'}"
            )
            
            if has_errors and error_message:
                log_message += f" | Error: {error_message}"
            
            # Log to both console and file
            logger = logging.getLogger('startup_reports')
            if has_errors:
                logger.warning(log_message)
            else:
                logger.info(log_message)
            
            # Send success response
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            
            response = {
                'status': 'ok',
                'message': 'Startup report received',
                'timestamp': timestamp
            }
            
            self.wfile.write(json.dumps(response).encode('utf-8'))
            
        except json.JSONDecodeError as e:
            logger = logging.getLogger('startup_reports')
            logger.error(f"Invalid JSON from {self.client_address[0]}: {e}")
            self.send_error(400, f"Invalid JSON: {e}")
            
        except Exception as e:
            logger = logging.getLogger('startup_reports')
            logger.error(f"Error processing request from {self.client_address[0]}: {e}")
            self.send_error(500, f"Internal server error: {e}")
    
    def log_message(self, format, *args):
        """Override to use our logger instead of stderr"""
        logger = logging.getLogger('http_server')
        logger.debug(format % args)


def setup_logging(log_dir: Path, verbose: bool = False):
    """Setup logging configuration"""
    
    # Create log directory if it doesn't exist
    log_dir.mkdir(parents=True, exist_ok=True)
    
    # Setup startup reports logger
    reports_logger = logging.getLogger('startup_reports')
    reports_logger.setLevel(logging.DEBUG)
    
    # Rotating file handler for startup reports (10MB max, keep 10 files)
    reports_file = log_dir / 'startup_reports.log'
    reports_handler = RotatingFileHandler(
        reports_file,
        maxBytes=10*1024*1024,  # 10MB
        backupCount=10
    )
    reports_handler.setLevel(logging.DEBUG)
    
    # Format: timestamp | level | message
    reports_format = logging.Formatter('%(message)s')
    reports_handler.setFormatter(reports_format)
    reports_logger.addHandler(reports_handler)
    
    # Console handler
    console_handler = logging.StreamHandler(sys.stdout)
    console_handler.setLevel(logging.DEBUG if verbose else logging.INFO)
    console_format = logging.Formatter('%(asctime)s | %(levelname)-7s | %(message)s')
    console_handler.setFormatter(console_format)
    reports_logger.addHandler(console_handler)
    
    # Setup HTTP server logger (less verbose)
    http_logger = logging.getLogger('http_server')
    http_logger.setLevel(logging.DEBUG if verbose else logging.WARNING)
    
    http_file = log_dir / 'http_server.log'
    http_handler = RotatingFileHandler(
        http_file,
        maxBytes=5*1024*1024,  # 5MB
        backupCount=3
    )
    http_handler.setLevel(logging.DEBUG)
    http_format = logging.Formatter('%(asctime)s | %(levelname)-7s | %(message)s')
    http_handler.setFormatter(http_format)
    http_logger.addHandler(http_handler)
    
    if verbose:
        http_logger.addHandler(console_handler)
    
    return reports_logger, http_logger


def run_server(port: int, log_dir: Path, verbose: bool = False):
    """Run the HTTP server"""
    
    # Setup logging
    reports_logger, http_logger = setup_logging(log_dir, verbose)
    
    reports_logger.info("=" * 70)
    reports_logger.info("Echoes of the Machine - Startup Report Server")
    reports_logger.info(f"Starting server on port {port}")
    reports_logger.info(f"Log directory: {log_dir.absolute()}")
    reports_logger.info("=" * 70)
    
    # Create server
    server_address = ('', port)
    httpd = HTTPServer(server_address, StartupReportHandler)
    
    reports_logger.info(f"Server listening on http://0.0.0.0:{port}")
    reports_logger.info("Waiting for startup reports from ESP32 devices...")
    reports_logger.info("Press Ctrl+C to stop")
    
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        reports_logger.info("\nShutting down server...")
        httpd.shutdown()
        reports_logger.info("Server stopped")


def main():
    """Main entry point"""
    
    parser = argparse.ArgumentParser(
        description='Startup Report Server for Echoes of the Machine',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Run on default port 8001 with logs in ./logs
  python3 startup_server.py
  
  # Run on custom port with custom log directory
  python3 startup_server.py --port 8080 --logdir /var/log/echoes
  
  # Run with verbose logging
  python3 startup_server.py --verbose
"""
    )
    
    parser.add_argument(
        '--port',
        type=int,
        default=8001,
        help='Port to listen on (default: 8001)'
    )
    
    parser.add_argument(
        '--logdir',
        type=Path,
        default=Path('./logs'),
        help='Directory for log files (default: ./logs)'
    )
    
    parser.add_argument(
        '--verbose',
        action='store_true',
        help='Enable verbose logging'
    )
    
    args = parser.parse_args()
    
    try:
        run_server(args.port, args.logdir, args.verbose)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
