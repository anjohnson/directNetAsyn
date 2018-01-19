# DirectNet PLC Simulation Network Protocol

* Version: Draft-2
* Date: 2018-01-19
* Author: Andrew Johnson

This document defines the messages that can be sent over the TCP connection between an IOC running directNetAsyn and a simulated PLC.
The on-the-wire protocol used by directNet itself was designed for slow, noisy serial connections; it uses binary encoding, and requires several round-trips for a single message transfer.
This simulation protocol uses ASCII encoding and normally only requires a single round-trip for each message, relying on the fact that the underlying TCP/IP message transport is fast and reliable.


## Architecture

The IOC software uses AsynDriver to open a TCP socket to the simulator at a specific host/IP address and port number.
Asyn will attempt to reconnect if the socket is closed by the simulator; the IOC will report errors from the directNetAsyn device support if it attempts to send messages while the socket is disconnected.

The directNet protocol is strictly master/slave, so all communications are initiated by the master (the IOC) and the simulator can only respond to the requests given.
Requests cannot be multiplexed, there can only be one operation active at a time.
However the driver does implement a timeout, so if the simulator does not respond to a request fast enough the driver can send a cancel message to the simulator and return a timeout error to the IOC.


## Messages

All messages are encoded in plain 7-bit ASCII and terminated with a newline '\n' (in practice implementations should accept any combination of CR and/or LF as the message terminator).
The newline will not be included in the descriptions below.
Each message starts with an upper-case letter from the following list, which indicates the type of message and hence what additional information follows on the line.

Unless described otherwise, numeric parameters are sent as fixed-width hexadecimal numbers (without an '0x'), separated by a single space character.
In the message descriptions below the fixed number of hex digits allowed is specified for each parameter.

The primary messages sent by the IOC to trigger I/O operations are:

```
    W - Write
    R - Read
```

There are several secondary messages which are also used:

```
    X - Cancel
    D - Data
    A - Ack
    N - Nak
```


### Write Message

```
    W <id:2> <cmd:2> <addr:4> <len:4>
```

A write message will be followed immediately by one or more Data messages containing the `<len>` bytes of data to be written, starting at the DirectNet address `<addr>`.

The `<id>` parameter is the slave ID of the PLC, set by the second argument to the `createDnAsynSimulatedPLC` command in the IOC's startup script.
This value can be ignored, but it could be used to simulate multiple PLCs for the same IOC, or to select between different types of PLC application that can be simulated by the simulator code.

The `<cmd>` parameter controls what data is to be written.
I know of 3 possible values for `<cmd>` when writing, and the `DNI` interactive command can generate any of them, but the device support code only ever uses the WRITEVMEM (0x81) value.
The full list is:

```
    WRITEVMEM   0x81    Write to V-memory
    WRITESPAD   0x86    Write to Scratchpad
    WRITEPROG   0x87    Write to Program (ladder logic)
```

The different `<cmd>` values control which of several possible address spaces within the PLC is to be written to, so `<addr>` is *not* the same as the PLCs internal VMEM address.
For the WRITEVMEM (0x81) command an offset of 1 is added to the PLC's VMEM address to generate `<addr>`, which is a word address so for adjacent 16-bit words the address increases by 1.

The `<len>` parameter gives the number of bytes that are to be written by the following Data messages, starting at `<addr>`.
The device support sends a separate Write message for each record, so most WRITEVMEM commands will only be 2 or 4 bytes long.

The simulator should normally only respond after seeing the final Data message, by returning an Ack.
If it unable to handle the request it may send a Nak anytime after seeing the Write message, but it should be prepared to accept and discard any remaining Data messages from the IOC which may already be in flight.


### Read Message

```
    R <id:2> <cmd:2> <addr:4> <len:4>
```

The `<cmd>` parameter controls what data is to be read.
I know of 6 possible values for `<cmd>` when reading, and the `DNI` interactive command can generate any of them, but the device support code only ever uses the READVMEM (0x01) value.
The full list is:

```
    READVMEM    0x01    Read from V-memory
    READINPS    0x02    Read Inputs
    READOUTS    0x03    Read Outputs
    READSPAD    0x06    Read Scratchpad
    READPROG    0x07    Read Program (ladder logic)
    READSTAT    0x09    Read Status
```

The different `<cmd>` values control which of several different address spaces within the PLC is to be read from, so `<addr>` is *not* the same as the PLCs internal VMEM address.
For the READVMEM (0x01) command an offset of 1 is added to the PLC's VMEM address to generate `<addr>`, which is a word address so for adjacent 16-bit words the address increases by 1.

The `<len>` parameter gives the number of bytes that are to be read, starting at `<addr>`.
The device support combines read requests from multiple records up to a maximum size, so most READVMEM commands will be for between 2 and 32 bytes of data.

A read message should normally cause the simulator to respond by returning one or more Data messages containing the requested data.
If it is unable to complete the request it may return a Nak instead, or it may start sending data packets but terminate the series early with a Nak as long as fewer than `<len>` bytes have been returned.
No more data may be returned for a Read operation after a Nak message.


### Data Message

```
    D <len:2> <data-00:2><data-01:2>...<data-1f:2>
```

A single Data message may contain up to 32 bytes of data (sending 0-byte messages is discouraged).
Multiple Data messages may need to be sent until the total number of bytes given in the initial Read or Write message has been reached.
Unlike the other message types there should be no spaces between the individual data values.

The `<len>` parameter gives the number of data byte values that follow in this message.
With the 32-byte limit, a single data message should be no longer than 70 characters from the initial 'D' to the trailing newline.

Partial Data messages should never be sent; completely assemble each Data message before sending any of it.

The PLC V-memory is structured as a series of 16-bit words, which are serialized for data transfers into two bytes with the LSB first (little-endian ordering).
This ordering is also used for storing 32-bit floating-point numbers in two adjacent words.
The PLC uses standard IEEE single-precision floating point numeric format.

Some newer PLCs may support combining 2 adjacent V-memory locations into a single 32-bit integer value, but directnetAsyn doesn't handle this data type yet.
PLCs can also process and store data in V-memory that is BCD encoded, but the directNetAsyn code doesn't handle this data either, the ladder logic should be converting BCD to/from 16-bit integers for communicating with the IOC.


### Ack Message

```
    A
```

An Ack message indicates an operation succeeded.
It contains no parameters.


### Nak Message

```
    N <error text:opt>
```

A Nak message indicates an operation failed.

The `<error text>` parameter is an optional string message which must contain only printable ASCII characters (no newlines or other control characters).
It is intended for development and debugging purposes only.

When a Nak is sent from the simulator to the IOC, the `<error text>` will be logged by the IOC using `asynPrint(ASYN_TRACE_ERROR)`.


### Cancel Message

```
    X
```

The IOC will send a Cancel message if an operation times out because the simulator doesn't respond fast enough, or if it becomes unable to handle further data from a Read command.
The timeout used will be 20 seconds, which should be long enough to avoid unintended timeouts from a busy simulator but short enough to avoid making a check of the timeout functionality too painful.

The simulator must send an Ack message when it sees a Cancel message, and not send any more read data after that.
After sending a Cancel the IOC will discard Data messages from its input buffer until it sees the Ack message (or the read times out) before sending any more commands.


## Examples of Message Exchanges

In the examples below, `'>'` indicates a message sent from the IOC to the simulator, and `'<'` indicates a response in the other direction (those characters are not sent).
Newline characters are indicated below by the line endings.


### Write V-memory

```
    > W 01 81 1235 0004
    > D 04 00100110
    < A
```

Data words written to V-memory addresses 0x1234 - 0x1235 (11064 - 11065 octal):

```
    1000 1001
```

### Read V-memory

```
    > R 01 01 1235 0010
    < D 10 00200120022003200420052006200720
```

Data words read from V-memory addresses 0x1234 - 0x123b (11064 - 11073 octal):

```
    2000 2001 2002 2003 2004 2005 2006 2007
```

### Longer read

```
    > R 01 01 1235 0044
    < D 20 00300130023003300430053006300730083009300a300b300c300d300e300f30
    < D 20 10301130123013301430153016301730183019301a301b301c301d301e301f30
    < D 04 20302130
```

Data words read from V-memory addresses 0x1234 - 0x1255 (11064 - 11125 octal):

```
    3000 3001 3002 3003 3004 3005 3006 3007
    3008 3009 300a 300b 300c 300d 300e 300f
    3010 3011 3012 3013 3014 3015 3016 3017
    3018 3019 301a 301b 301c 301d 301e 301f
    3020 3021
```

### Rejected write

```
    > W 01 81 8000 0002
    < N Address out of range
    > D 02 3412
```

### Rejected read

```
    > R 01 01 8000 0010
    < N Address out of range
```

### Cancelled read

```
    > R 01 01 1235 0080
    < D 20 00400140024003400440054006400740084009400a400b400c400d400e400f40
    > X
    < D 20 10401140124013401440154016401740184019401a401b401c401d401e401f40
    < A
```

Data words read from V-memory addresses 0x1234 - 0x1253 (11064 - 11123 octal):

```
    4000 4001 4002 4003 4004 4005 4006 4007
    4008 4009 400a 400b 400c 400d 400e 400f
    4010 4011 4012 4013 4014 4015 4016 4017
    4018 4019 401a 401b 401c 401d 401e 401f
```

### Partial read

```
    > R 01 01 4291 0040
    < D 20 00500150025003500450055006500750085009500a500b500c500d500e500f50
    < N Address out of range
```

Data words read from V-memory addresses 0x4290 - 0x429f (41220 - 41237 octal):

```
    5000 5001 5002 5003 5004 5005 5006 5007
    5008 5009 500a 500b 500c 500d 500e 500f
```

For some PLCs the address 41237 octal is the last available location in V-memory, hence the above read stopped after it reached that address.

