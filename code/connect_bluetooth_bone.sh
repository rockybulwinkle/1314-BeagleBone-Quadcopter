#!/bin/sh
TERM=xterm
SHELL=/bin/bash
SSH_TTY=/dev/pts/0
USER=root
PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin
PWD=/home/root
EDITOR=/bin/vi
NODE_PATH=/usr/lib/node_modules
PS1=\[`if [[ $? = 0 ]]; then echo '\e[32m\u@beaglebone\e[0m'; else echo '\e[31m\u@beaglebone\e[0m' ; fi`:\w\n$ 
SHLVL=1
HOME=/home/root
LOGNAME=root
_=/usr/bin/env

# Make sure to have rfcomm loaded
modprobe rfcomm

# Turn on and reset bluetooth dongle
#hciconfig hci0 up
#hciconfig hci0 reset

# Accept incoming connections (in background)
rfcomm listen hci0 11 & 
#rfcomm watch 0 1 &
# Loop forever
#while true
#do
  # Wait for our socket to pop in
  while [ ! -c /dev/rfcomm0 ]
  do
    sleep 5
  done

  # Present a login shell
#  getty -n -l /bin/bash /dev/rfcomm0
#done
echo "starting pppd"
rm -f /var/lock/*rfcomm0*
#921600
until pppd proxyarp mtu 1280 persist nodeflate noauth lcp-echo-interval 10 crtscts lock 10.10.1.2:10.10.1.1 /dev/rfcomm0 1000000000000
do
	echo "retrying"
	sleep 1
done
#set the gateway to host's ip

until route add default gw 10.10.1.1
do
	echo "retrying"
	sleep 1
done

#populate the nameservers.  Might need to change when you leave rose.
echo "domain rose-hulman.edu
nameserver 137.112.18.59
nameserver 137.112.5.28
nameserver 137.112.4.196
nameserver 137.112.12.11
">>/etc/resolv.conf
