zenfs_SOURCES = fs/fs_zenfs.cc fs/zbd_zenfs.cc fs/io_zenfs.cc
zenfs_HEADERS = fs/fs_zenfs.h fs/zbd_zenfs.h fs/io_zenfs.h fs/zbd_stat.h
zenfs_LDFLAGS = -lzbd -laio -u zenfs_filesystem_reg
