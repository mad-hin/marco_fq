# A modified sch_fq module

The original code is the `sch_fq.c` of the kernel version `5.15`

## Modified parts

- Improved QoS with prioritized bandwidth allocation.
- Better congestion control, reducing latency and bufferbloat.
- Greater flexibility for flow and scheduling management.
- Enhanced monitoring with detailed traffic statistics.

## The kernel module

### How to compile the module

1. `cd tc_sch/`
2. run `make` to compile the module

### How to load the module

1. `cd tc_sch/`
2. run `make load` to load the module (require `sudo`)

### How to unload the module

1. `cd tc_sch/`
2. run `make unload` to unload the module (require `sudo`)

## The qdisc

### Load to qdisc

1. run `interface.sh`
2. `cd tc_q/`
   1. run `setup.sh` if first time setup
3. run `cd iproute2`
4. run `sudo TC_LIB_DIR='./tc' tc qdisc add dev veth0 root marco_fq limit 100 & sudo TC_LIB_DIR='./tc' tc qdisc add dev enp0s3 root marco_fq limit 100`

### Unload the qdisc

1. `cd tc_q/`
2. run `unload.sh` to unload the module (It will change the qdisc to `fq`)

## Testing

You may refer to [this](test/README.md)
