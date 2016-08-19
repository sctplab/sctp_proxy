# A Generic SCTP Proxy 

## Supported Platforms and Compilation
The proxy supports all operating systems with kernel SCTP support.
The following table shows how to compile the code.

|OS      | Command                                    |
|:-------|:-------------------------------------------|
|FreeBSD |`cc -o proxy -pedantic -pthread proxy.c`    |
|Linux   |                                            |
|Solaris |`gcc -o proxy -lnsl -lsocket -lsctp proxy.c |

## Command Line Arguments
