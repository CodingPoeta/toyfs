# Toyfs

A simple toy kernel file system from scratch.

## Start

```bash
make
sudo insmod toyfs.ko
sudo mount -t toyfs none /mnt
# do something
sudo umount /mnt
sudo rmmod toyfs
```

## TODO

- [x] Implement basic file system in memory
- [ ] Support more operations
- [ ] Support block device
- [ ] Support page cache
- [ ] ...
