import socket

# Define the IP address and the Port Number
ip = input("Please type the ip where you want to send")
port = 12345

# Create a UDP socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# Bind the socket to the address and port
sock.bind((ip, port))

# Listen for incoming datagrams
while True:
    data, addr = sock.recvfrom(1024)  # buffer size is 1024 bytes
    print(f"received message: {data.decode()} from {addr}")
    message = f"Received Packet"
    sock.sendto(message.encode(), (ip, port))

# Close the socket
sock.close()