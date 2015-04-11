Installation steps
-------------------

Requirements:

  * Linux kernel version 3.7 onwards
  * `python` 2.6.6 onwards

Run following commands as **root**

  1. `eio_cli` installation

        cp CLI/eio_cli /sbin/
        chmod 700 /sbin/eio_cli

  2. Man page
    Copy the `eio_cli.8` file under the `man8` subdirectory of the `man` directory
    (usually `/usr/share/man/man8/`).

  3. Driver installation
    In the `Driver/enhanceio` subdirectory run (as root)

        make && make install

    The use of `sudo` is currently not supported and will fail with error
    `No rule to make target `arch/x86/syscalls/syscall_32.tbl', needed by `arch/x86/syscalls/../include/generated/asm/syscalls_32.h'.  Stop.`.

  4. manually load modules by running
  
        modprobe enhanceio_fifo
        modprobe enhanceio_lru
        modprobe enhanceio
   
   You can now create `enhanceio` caches using the utility `eio_cli`. Please 
   refer to `Documents/Persistence.txt` for information about making a cache
   configuration persistent across reboot.
