# A Generic SCTP Proxy 

## Supported Platforms and Compilation
The proxy supports all operating systems with kernel SCTP support.
The following table shows how to compile the code.

|OS      | Compile Command                            |
|:-------|:-------------------------------------------|
|FreeBSD |`cc -o proxy -pthread proxy.c`              |
|Linux   |`gcc -o proxy -lsctp proxy.c`               |
|Solaris |`gcc -o proxy -lnsl -lsocket -lsctp proxy.c`|

On Linux you must have installed the `libsctp-dev` package.

## Command Line Arguments
