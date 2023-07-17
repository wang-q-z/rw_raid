# qemu
```
sudo qemu-system-x86_64 -m 8192  -smp 4 -cpu host -enable-kvm  -nographic -kernel ../linux-5.11.1/arch/x86_64/boot/bzImage -hda ubuntu.img -hdb /dev/nvme4n1 -hdc /dev/nvme5n1 -hdd /dev/nvme6n1  -append "console=tty0 console=ttyS0 root=/dev/sda rootfstype=ext4  rw  nokaslr init=/bin/sh" -s
```

# md
```
mdadm --create /dev/md0 --level=5 --raid-devices=3 /dev/sdb /dev/sdc /dev/sdd -c 64 --assume-clean && fio  --filename=/dev/md0  --rw=write --bs=1k --size=100M --direct=1 --numjobs=1 --runtime=1m --group_reporting --name=test-write
```