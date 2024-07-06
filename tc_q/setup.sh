git clone https://github.com/iproute2/iproute2.git
cp q_marco_fq.c iproute2/tc 
cd iproute2
make TCSO=q_marco_fq.so