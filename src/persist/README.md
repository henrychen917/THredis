# Append-only persistence

AOF producers, physical stream ownership, syncing, manifest-based rewrite, and
replay live here. Continuous mutation logging has different ordering and
durability duties from snapshot capture, while reusing snapshots for AOF bases.
