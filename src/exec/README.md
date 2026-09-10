# Execution transport

`Op` and the SPSC queue primitives carry work and control messages between
threads. They are separate from the worker loops and command handlers so queue
publication, capacity, and retirement rules have one implementation.
