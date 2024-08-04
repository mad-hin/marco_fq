import socket
import time

server_ip = "10.0.2.4"
server_port = 12345
server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)  # Use SOCK_STREAM for TCP
server_socket.bind((server_ip, server_port))
server_socket.listen(1)  # Listen for incoming connections
print(f"Server started at {server_ip}:{server_port}")
cnt = 0
last_send_time = None
while True:
    conn, addr = server_socket.accept()  # Accept a connection
    data = conn.recv(1024)  # Receive data from the client
    cnt += 1
    message = "ACK" + data.decode()
    if last_send_time is not None:
        time_difference = (time.time() - last_send_time)*1000
        print(f"Time difference with the last send: {time_difference} ms")
    last_send_time = time.time()
    if cnt > 100:
        time.sleep(0.5)
    conn.send(message.encode())  # Send data to the client

conn.close()  # Close the connection