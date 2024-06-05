import socket
import time

server_ip = "10.0.2.5"
server_port = 12345

client_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for i in range(1,101):
    message =f"Packet {i}"
    message = message.encode()
    start = time.time()
    client_socket.sendto(message, (server_ip, server_port))
    data, server = client_socket.recvfrom(1024)
    end = time.time()
    rtt = (end - start) * 1000  # convert to milliseconds
    print(f"Received {data} from {server}, RTT = {rtt} ms")
