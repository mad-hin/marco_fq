# A modified sch_fq module

The original code is the `sch_fq.c` of the kernel version `5.15`

## Modified parts

- added a debug message in the `fq_module_init` and `fq_module_exit` to see have the module loaded or not
- changed the id name in `line 1103`
- changed the function names (added `marco_` in front of the function names)

## Load to qdisc

Current process: cannot load to qdisc

### Possible reason

`openat(AT_FDCWD, "/usr/lib/tc//q_marco_fq.so", O_RDONLY|O_CLOEXEC) = -1 ENOENT (No such file or directory)`
