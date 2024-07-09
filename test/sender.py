import socket
import time
import threading

server_ip = "10.0.2.5"
server_port = 12345

def send_packet(i):
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    message =f"Packet {i}"
    message = message.encode()
    start = time.time()
    client_socket.sendto(message, (server_ip, server_port))
    data, server = client_socket.recvfrom(1024)
    end = time.time()
    rtt = (end - start) * 1000  # convert to milliseconds
    if rtt > 10:
        print(f"Received {data} from {server}, RTT = {rtt} ms")

threads = []

for i in range(1,10000):
    thread = threading.Thread(target=send_packet, args=(i,))
    thread.start()
    threads.append(thread)

for thread in threads:
    thread.join()
