import socket
import time

server_ip = "10.0.2.4"
server_port = 12345
server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
server_socket.bind((server_ip, server_port))
print(f"Server started at {server_ip}:{server_port}")
cnt = 0
last_send_time = None
while True:
    data, addr = server_socket.recvfrom(1024)
    cnt += 1
   # print(f"Received {data} from {addr}")
    message = "ACK" + data.decode()
    if last_send_time is not None:
        time_difference = (time.time() - last_send_time)*1000
        # if time_difference > 10:
        print(f"Time difference with the last send: {time_difference} ms")
    last_send_time = time.time()
    if cnt > 100:
        time.sleep(0.5)
    server_socket.sendto(message.encode(), addr)
