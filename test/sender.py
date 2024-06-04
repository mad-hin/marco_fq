import socket

# Define the IP address and the Port Number
ip = input("Please type the ip where you want to send")
port = 12345

# Create a UDP socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# Send 10 UDP packets
for i in range(10):
    message = f"Packet {i+1}"
    sock.sendto(message.encode(), (ip, port))

# Close the socket
sock.close()