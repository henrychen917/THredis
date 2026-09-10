# Commands

Command tables and handlers, expanded type representations, ACLs, scripts and
functions, transactions, blocking, and cross-shard execution live here. Each
feature owns its semantics and registers through the common command interface;
the transport loops use that metadata rather than owning the command grammar.
