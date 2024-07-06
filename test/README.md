# Readme for testing

In this testing folder, it is trying to preform sending some UDP packets from A to B via C, where C is the machine that loaded this qdisc

Note that there are no restriction on the linux kernel version except for Host C, Host C must be in `5.15`

## Pre-request

- Create 3 VMs under a NAT Network
- Clone this repo to all 3 machine

## Host A (sender)

1. `cd` to this folder
   1. run `setup.sh`
   2. run `pip3 install -r requirements.txt`
2. run `source test_env/bin/activate`
   1. modify the ip address in `sender.py` if needed
3. run `sudo ip route add <Host B ip address> via <Host C ip address>`
4. run `python3 sender.py` Note that this should be run after the `receiver.py` is running in Host B

## Host B (receiver)

1. `cd` to this folder
   1. run `setup.sh`
   2. run `pip3 install -r requirements.txt`
2. run `source test_env/bin/activate`
   1. modify the ip address in `receiver.py` if needed
3. run `sudo ip route add <Host A ip address> via <Host C ip address>`
4. run `python3 receiver.py`

## Host C (router)

1. run `sudo sysctl -w net.ipv4.ip_forward=1`
2. set up the qdisc as listed in the [README.md](../README.md)
