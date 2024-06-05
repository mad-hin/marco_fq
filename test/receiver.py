import socket

server_ip = "10.0.2.5"
server_port = 12345
server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
server_socket.bind((server_ip, server_port))
print(f"Server started at {server_ip}:{server_port}")
while True:
    data, addr = server_socket.recvfrom(1024)
    print(f"Received {data} from {addr}")
    message = "ACK" + data.decode()
    server_socket.sendto(message.encode(), addr)
