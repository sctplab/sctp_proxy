# A Generic SCTP Proxy 

## Features
The proxy preserves message boundaries, the ordered/unordered property, the payload protocol identifier, and the stream identifier.
It distributes the incoming SCTP associations on multiple servers in a round robin fashion.

## Supported Platforms and Compilation
The proxy supports all operating systems with kernel SCTP support.
The following table shows how to compile the code.

|OS      | Compile Command                            |
|:-------|:-------------------------------------------|
|FreeBSD |`cc -o proxy -pthread proxy.c`              |
|Linux   |`gcc -o proxy -lsctp proxy.c`               |
|Solaris |`gcc -o proxy -lnsl -lsocket -lsctp proxy.c`|

On Linux you must have installed the `libsctp-dev` package.
This provides the required header files and library.

## Command Line Arguments

* `-i number_of_incoming_streams`
* `-o number_of_outgoing_stream`
* `-L address1,address2,...,addressN:port`
* `-X address1,address2,...,addressN`
* `-S address1,address2,...,addressN:port`

## Example
```
proxy -i 1024 -o 1024 -L 127.0.0.1,::1:5001 -X 127.0.0.1,::1 -S 127.0.0.1,::1:6001 -S 127.0.0.1,::1:6002
```
