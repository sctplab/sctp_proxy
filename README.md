# A Generic SCTP Proxy 

## Features
The proxy preserves message boundaries, the ordered/unordered property, the payload protocol identifier, and the stream identifier.
It distributes the incoming SCTP associations to multiple servers in a round robin fashion using multiple threads.

## Supported Platforms and Compilation
The proxy supports all operating systems with kernel SCTP support.
The following table shows how to compile the code.

|OS      | Compile Command                            |
|:-------|:-------------------------------------------|
|FreeBSD |`cc -o proxy -pthread proxy.c`              |
|Linux   |`gcc -o proxy -pthread proxy.c -lsctp`      |
|Solaris |`gcc -o proxy proxy.c -lsocket -lnsl -lsctp`|

On Linux, you must have installed the `libsctp-dev` package.
This provides the required header files and the required library.

## Command Line Arguments

* `-4`
* `-6`
* `-i number_of_incoming_streams`
* `-o number_of_outgoing_streams`
* `-L address_1,address_2,...,address_N:port`
* `-X address_1,address_2,...,address_N`
* `-S address_1,address_2,...,address_N:port`

## Example
```
proxy -i 1024 -o 1024 -L 127.0.0.1,::1:5001 -X 127.0.0.1,::1 -S 127.0.0.1,::1:6001 -S 127.0.0.1,::1:6002
```
