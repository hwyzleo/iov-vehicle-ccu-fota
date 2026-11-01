#!/usr/bin/env python3
"""
Mock DIAG Service for testing CCU-FOTA.
Listens on port 30501 and responds to SOME/IP requests with mock data.
"""

import socket
import struct
import sys
import time

# SOME/IP header format (big-endian as per SOME/IP standard):
# uint16 service_id
# uint16 method_id
# uint32 length (including header)
# uint16 client_id
# uint16 session_id
# uint8 protocol_version
# uint8 interface_version
# uint8 message_type (0x00 = request, 0x80 = response)
# uint8 return_code (0x00 = ok)
SOMEIP_HEADER_FORMAT = '>HHI H H B B B B'
SOMEIP_HEADER_SIZE = struct.calcsize(SOMEIP_HEADER_FORMAT)

# Service IDs (aligned with iov-vehicle-ccu-diag project)
DIAG_SERVICE_ID = 0x7725
DIAG_INSTANCE_ID = 0x0001

# Method IDs (aligned with CCU-DIAG-DSN-CR-006)
METHOD_READ_VIN = 0x0001
METHOD_COLLECT_VEHICLE_INVENTORY = 0x0002
METHOD_GET_ECU_VERSION = 0x0003
METHOD_GET_REGISTRY_VERSION = 0x0004

# Mock data
MOCK_VIN = "12345678901234567"
MOCK_REGISTRY_VERSION = "1.0.0"

def build_someip_response(service_id, method_id, client_id, session_id, payload, return_code=0x00):
    """Build a SOME/IP response message."""
    # Message type 0x80 = response
    message_type = 0x80
    protocol_version = 0x01
    interface_version = 0x01
    
    # SOME/IP standard: length = 8 (from length field onwards) + payload_size
    length = 8 + len(payload)
    
    # Pack header (big-endian as per SOME/IP standard)
    header = struct.pack(SOMEIP_HEADER_FORMAT,
                        service_id,
                        method_id,
                        length,
                        client_id,
                        session_id,
                        protocol_version,
                        interface_version,
                        message_type,
                        return_code)
    
    return header + payload

def handle_request(data, client_address):
    """Handle a SOME/IP request."""
    if len(data) < SOMEIP_HEADER_SIZE:
        print(f"[ERROR] Received data too short: {len(data)} bytes")
        return None
    
    # Unpack header
    service_id, method_id, length, client_id, session_id, \
    protocol_version, interface_version, message_type, return_code = \
        struct.unpack(SOMEIP_HEADER_FORMAT, data[:SOMEIP_HEADER_SIZE])
    
    # Debug: print raw bytes
    print(f"[DEBUG] Raw header bytes: {data[:SOMEIP_HEADER_SIZE].hex()}", flush=True)
    print(f"[REQUEST] From {client_address}: service=0x{service_id:04x}, method=0x{method_id:04x}, "
          f"session=0x{session_id:04x}, message_type=0x{message_type:02x}", flush=True)
    
    # Extract payload if any
    payload = data[SOMEIP_HEADER_SIZE:] if len(data) > SOMEIP_HEADER_SIZE else b''
    
    # Handle different methods
    if method_id == METHOD_READ_VIN:
        print(f"[HANDLER] READ_VIN request")
        # Return mock VIN (17 characters)
        response_payload = MOCK_VIN.encode('utf-8')
        return build_someip_response(service_id, method_id, client_id, session_id, response_payload)
    
    elif method_id == METHOD_GET_REGISTRY_VERSION:
        print(f"[HANDLER] GET_REGISTRY_VERSION request")
        response_payload = MOCK_REGISTRY_VERSION.encode('utf-8')
        return build_someip_response(service_id, method_id, client_id, session_id, response_payload)
    
    elif method_id == METHOD_GET_ECU_VERSION:
        print(f"[HANDLER] GET_ECU_VERSION request")
        # Parse ECU ID from payload
        if payload:
            ecu_id = payload.decode('utf-8', errors='ignore')
            print(f"[HANDLER] ECU ID: {ecu_id}")
            # Return mock ECU version data
            mock_data = f"{ecu_id},PN{ecu_id[3:] if len(ecu_id) > 3 else '001'},1.0.0,HW1.0"
            response_payload = mock_data.encode('utf-8')
            return build_someip_response(service_id, method_id, client_id, session_id, response_payload)
        else:
            # No ECU ID provided, return error
            return build_someip_response(service_id, method_id, client_id, session_id, b'', return_code=0x01)
    
    elif method_id == METHOD_COLLECT_VEHICLE_INVENTORY:
        print(f"[HANDLER] COLLECT_VEHICLE_INVENTORY request")
        # This method would return the full inventory, but for simplicity we'll return success
        # The actual implementation would call getVin and getEcuVersion internally
        return build_someip_response(service_id, method_id, client_id, session_id, b'')
    
    else:
        print(f"[HANDLER] Unknown method: 0x{method_id:04x}")
        return build_someip_response(service_id, method_id, client_id, session_id, b'', return_code=0x01)

def main():
    """Main function to run the mock DIAG service."""
    host = '0.0.0.0'
    port = 30501
    
    print(f"[STARTING] Mock DIAG Service on {host}:{port}", flush=True)
    print(f"[INFO] Service ID: 0x{DIAG_SERVICE_ID:04x}", flush=True)
    print(f"[INFO] Mock VIN: {MOCK_VIN}", flush=True)
    print(f"[INFO] Mock Registry Version: {MOCK_REGISTRY_VERSION}", flush=True)
    
    # Create TCP socket
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server_socket.bind((host, port))
        server_socket.listen(5)
        print(f"[LISTENING] Waiting for connections on port {port}", flush=True)
        
        while True:
            # Accept connection
            client_socket, client_address = server_socket.accept()
            print(f"[CONNECTION] New connection from {client_address}", flush=True)
            
            try:
                # Handle multiple requests on the same connection
                while True:
                    # Receive data
                    data = client_socket.recv(4096)
                    if not data:
                        print(f"[DISCONNECT] Client {client_address} disconnected", flush=True)
                        break
                    
                    print(f"[DEBUG] Received {len(data)} bytes from {client_address}", flush=True)
                    
                    # Handle request
                    response = handle_request(data, client_address)
                    
                    if response:
                        # Send response
                        client_socket.sendall(response)
                        print(f"[RESPONSE] Sent {len(response)} bytes to {client_address}", flush=True)
                    else:
                        print(f"[ERROR] No response generated for request from {client_address}", flush=True)
                        
            except ConnectionResetError:
                print(f"[ERROR] Connection reset by {client_address}", flush=True)
            except Exception as e:
                print(f"[ERROR] Exception handling client {client_address}: {e}", flush=True)
            finally:
                client_socket.close()
                
    except KeyboardInterrupt:
        print("\n[SHUTDOWN] Shutting down Mock DIAG Service", flush=True)
    except Exception as e:
        print(f"[ERROR] Failed to start service: {e}", flush=True)
    finally:
        server_socket.close()

if __name__ == "__main__":
    main()