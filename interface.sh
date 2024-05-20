#! /bin/bash
# echo "Load dummy kernel module"
# sudo modprobe dummy
# echo "Create virtual interface"
# sudo  ip link add veth0 type dummy
# echo "The virtual interface (veth0) have been created"
# echo "Show interface"
# ip link show

# IPTABLES=/sbin/iptables

# LANIF="enp0s3"
# WANIF="veth0"

# # enable ip forwarding in the kernel
# echo 'Enabling Kernel IP forwarding...'
# /bin/echo 1 > /proc/sys/net/ipv4/ip_forward

# # flush rules and delete chains
# echo 'Flushing rules and deleting existing chains...'
# $IPTABLES -F
# $IPTABLES -X

# # enable masquerading to allow LAN internet access
# echo 'Enabling IP Masquerading and other rules...'
# $IPTABLES -t nat -A POSTROUTING -o $LANIF -j MASQUERADE
# $IPTABLES -A FORWARD -i $LANIF -o $WANIF -m state --state RELATED,ESTABLISHED -j ACCEPT
# $IPTABLES -A FORWARD -i $WANIF -o $LANIF -j ACCEPT

# $IPTABLES -t nat -A POSTROUTING -o $WANIF -j MASQUERADE
# $IPTABLES -A FORWARD -i $WANIF -o $LANIF -m state --state RELATED,ESTABLISHED -j ACCEPT
# $IPTABLES -A FORWARD -i $LANIF -o $WANIF -j ACCEPT

# echo 'Let the virtual interface up'
# ip link set dev veth0 up
# echo 'Done.'

sudo modprobe ifb
echo "Create virtual interface"
sudo  ip link add veth0 type ifb
ip link set dev veth0 up
tc qdisc add dev enp0s3 ingress
tc filter add dev enp0s3 parent ffff: protocol all u32 match u8 0 0 action mirred egress redirect dev veth0
