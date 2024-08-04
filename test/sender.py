import socket
import time

server_ip = "10.0.2.4"
server_port = 12345

for i in range(0,110):
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)  # Use SOCK_STREAM for TCP
    client_socket.connect((server_ip, server_port))  # Establish a connection
    if i < 10:
        message =f"0{i}"
    else:
        message =f"{i}"
    message = message.encode()
    start = time.time()
    client_socket.send(message)  # Send data to the server
    data = client_socket.recv(1024)  # Receive data from the server
    end = time.time()
    rtt = (end - start) * 1000  # convert to milliseconds
    # if rtt > 10:
    print(f"Received {data} from {server_ip}, RTT = {rtt} ms")
    client_socket.close()  # Close the connection

threads = []

# for i in range(1,100):
#     thread = threading.Thread(target=send_packet, args=(i,))
#     thread.start()
#     threads.append(thread)

# for thread in threads:
#     thread.join()
