cd iproute2
sudo TC_LIB_DIR='./tc' tc qdisc add dev veth0 root marco_fq limit 100 & sudo TC_LIB_DIR='./tc' tc qdisc add dev enp0s3 root marco_fq limit 100
tc qdisc list
