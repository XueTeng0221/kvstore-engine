#Replication behavior

## LiveSync identity and retention

LiveSync identifies an event by the length-delimited pair `(origin node ID, event ID)`. Relays keep
that identity unchanged while assigning no new logical event identity. The local replication offset
continues to describe ordered application on the single-primary chain.

Each process retains at most 4096 identities for ten minutes. Capacity eviction removes the oldest
identity;
TTL expiry makes the identity eligible again.The window is intentionally process -
    local and is empty after restart.A restarted relay first installs an upstream snapshot,
    resets its local backlog,
    and disconnects downstream peers so they reconnect at a clean snapshot boundary.Therefore an
            identity forgotten across restart can be observed again,
    but it is not concatenated with or
        replayed into an existing downstream transfer
                .

            Events are recorded in the deduplication window before application
                .Failed application removes those records so reconnect can retry.Successful events
                    are republished to the relay backlog with their original origin node and
            event ID.A downstream snapshot reset closes existing downstream peer connections;
reconnect starts a new transfer ID and prevents old and replacement chunks from mixing.

    ##Execution backends

        The replication executor controls full -
    sync installation and incremental application. `pthread` and the project -
    owned `ntyco` mode use bounded worker queues.The current `reactor` executor runs
        apply inline on the epoll owner thread to preserve event -
    loop ownership and avoid synchronous self -
    post deadlock.The current `proactor` adapter uses explicit submit /
        completion ownership but is completed inline by server assembly;
it is not an io_uring implementation.The `ntyco` name currently selects a project -
    owned cooperative worker queue,
    not the third - party NtyCo coroutine runtime.These names are execution - policy hooks only;
genuine io_uring proactor and NtyCo coroutine integrations remain unimplemented
    and must not be represented as transport backends.All modes share queue bounds,
    drain accepted work during shutdown, reject new work after shutdown,
    and isolate callback exceptions.
