# OxenMQ - high-level zeromq-based message passing for network-based projects

This C++17 library contains an abstraction layer around ZeroMQ to provide a high-level interface to
authentication, RPC, and message passing.  It is used extensively within Oxen projects (hence the
name) as the underlying communication mechanism of SN-to-SN communication ("quorumnet"), the RPC
interface used by wallets and local daemon commands, communication channels between oxend and
auxiliary services (storage server, lokinet), and also provides local multithreaded job scheduling
within a process.

Messages channels can be encrypted (using x25519) or not -- however opening an encrypted channel
requires knowing the server pubkey.  Within Oxen, all SN-to-SN traffic is encrypted, and other
traffic can be encrypted as needed.

This library makes minimal use of mutexes, and none in the hot paths of the code, instead mostly
relying on ZMQ sockets for synchronization; for more information on this (and why this is generally
much better performing and more scalable) see the ZMQ guide documentation on the topic.

## Basic message structure

OxenMQ messages come in two fundamental forms: "commands", consisting of a command named and
optional arguments, and "requests", consisting of a request name, a request tag, and optional
arguments.

All channels are capable of bidirectional communication, and multiple messages can be in transit in
either direction at any time.  OxenMQ sets up a "listener" and "client" connections, but these only
determine how connections are established: once established, commands can be issued by either party.

The command/request string is one of two types:

`category.command` - for commands/requests registered by the OxenMQ caller (e.g. oxend).  Here
`category` must be at least one character not containing a `.` and `command` may be anything.  These
categories and commands are registered according to general function and authentication level (more
on this below).  For example, for oxend categories are:

- `system` - is for RPC commands related to the system administration such as mining, getting
  sensitive statistics, accessing SN private keys, remote shutdown, etc.
- `sn` - is for SN-to-SN communication such as blink quorum and uptime proof obligation votes.
- `blink` - is for public blink commands (i.e. blink submission) and is only provided by nodes
  running as service nodes.
- `blockchain` - is for remote blockchain access such as retrieving blocks and transactions as well
  as subscribing to updates for new blocks, transactions, and service node states.

The difference between a request and a command is that a request includes an additional opaque tag
value which is used to identify a reply.  For example you could register a `general.backwards`
request that takes a string that receives a reply containing that string reversed.  When invoking
the request via OxenMQ you provide a callback to be invoked when the reply arrives.  On the wire
this looks like:

    <<< [general.backwards] [v71.&a] [hello world]
    >>> [REPLY] [v71.&a] [dlrow olleh]

where each [] denotes a message part and `v71.&a` is a unique randomly generated identifier handled
by OxenMQ (both the invoker and the recipient code only see the `hello world`/`dlrow olleh` message
parts).

In contrast, regular registered commands have no identifier or expected reply callback.  For example
you could register a `general.pong` commands that takes an argument and prints it out.  So requests
and output would look like this:

    >>> [general.pong] [hi]
    hi
    >>> [general.pong] [there]
    there

You could also create a `ping` command that instructs someone to pong you with a random word -- i.e.
give him a ping and she sends you a pong command:

    <<< [general.ping]
    >>> [general.pong] [omg]
    omg

Although this *looks* like a reply it isn't quite the same because there is no connection between
the ping and the pong (and, as above, pongs can be issued directly).  In particular this means if
you send multiple pings to the same recipient:

    <<< [general.ping]
    <<< [general.ping]
    >>> [general.pong] [world]
    >>> [general.pong] [hello]

you would have no way to know whether the first pong is in reply to the first or second ping.  We
could amend this to include a number to be echoed back:

    <<< [general.ping] [1]
    <<< [general.ping] [2]
    >>> [general.pong] [2] [world]
    >>> [general.pong] [1] [hello]

and now, in the pong, we could keep track of which number goes with which outgoing ping.  This is
the basic idea behind using a reply instead of command, except that you don't register the `pong`
command at all (there is a generic "REPLY" command for all replies), and the index values are
handled for you transparently.

## Command arguments

Optional command/request arguments are always strings on the wire.  The OxenMQ-using developer is
free to create whatever encoding she wants, and these can vary across commands.  For example
`wallet.tx` might be a request that returns a transaction in binary, while `wallet.tx_info` might
return tx metadata in JSON, and `p2p.send_tx` might encode tx data and metadata in a bt-encoded
data string.

No structure at all is imposed on message data to allow maximum flexibility; it is entirely up to
the calling code to handle all encoding/decoding duties.

Internal commands passed between OxenMQ-managed threads use either plain strings or bt-encoded
dictionaries.  See `oxenmq/bt_serialize.h` if you want a bt serializer/deserializer.

## Sending commands

Sending a command to a peer is done by using a connection ID, and generally falls into either a
`send()` method or a `request()` method.

    lmq.send(conn, "category.command", "some data");
    lmq.request(conn, "category.command", [](bool success, std::vector<std::string> data) {
        if (success) { std::cout << "Remote replied: " << data.at(0) << "\n"; } });

The connection ID generally has two possible values:

- a string containing a service node pubkey.  In this mode OxenMQ will look for the given SN in
  already-established connections, reusing a connection if one exists.  If no connection already
  exists, a new connection to the given SN is attempted (this requires constructing the OxenMQ
  object with a callback to determine SN remote addresses).
- a ConnectionID object, typically returned by the `connect_remote` method (although there are other
  places to get one, such as from the `Message` object passed to a command: see the following
  section).

    ```C++
    // Send to a service node, establishing a connection if necessary:
    std::string my_sn = ...; // 32-byte pubkey of a known SN
    lmq.send(my_sn, "sn.explode", "{ \"seconds\": 30 }");

    // Connect to a remote by address then send it something
    auto conn = lmq.connect_remote("tcp://127.0.0.1:4567",
        [](ConnectionID c) { std::cout << "Connected!\n"; },
        [](ConnectionID c, string_view f) { std::cout << "Connect failed: " << f << "\n" });
    lmq.request(conn, "rpc.get_height", [](bool s, std::vector<std::string> d) {
        if (s && d.size() == 1)
            std::cout << "Current height: " << d[0] << "\n";
        else
            std::cout << "Timeout fetching height!";
    });
    ```

## Command invocation

The application registers categories and registers commands within these categories with callbacks.
The callbacks are passed a OxenMQ::Message object from which the message (plus various connection
information) can be obtained.  There is no structure imposed at all on the data passed in subsequent
message parts: it is up to the command itself to deserialize however it wishes (e.g. JSON,
bt-encoded, or any other encoding).

The Message object also provides methods for replying to the caller.  Simple replies queue a reply
if the client is still connected.  Replies to service nodes can also be "strong" replies: when
replying to a SN that has closed connection with a strong reply we will attempt to reestablish a
connection to deliver the message.  In order for this to work the OxenMQ caller must provide a
lookup function to retrieve the remote address given a SN x25519 pubkey.

### Callbacks

Invoked command functions are always invoked with exactly one arguments: a non-const OxenMQ::Message
reference from which the connection info, OxenMQ object, and message data can be obtained.

The Message object also contains a `ConnectionID` object as the public `conn` member; it is safe to
take a copy of this and then use it later to send commands to this peer.  (For example, a wallet
might issue a command to a node requesting that it be sent any new transactions that arrive; the
node could store a copy of the ConnectionID, then use these copies when any such transaction
arrives).

## Authentication

Each category has access control consisting of three values:

- Auth level, one of:
  - None - no authentication required at all, any remote client may invoke this command
  - Basic - this requires a basic authentication level (None access is implied)
  - Admin - this requires administrative access (Basic access is implied)
- ServiceNode (bool) - if true this requires that the remote connection has proven its identity as
  an active service node (via its x25519 key).
- LocalServiceNode (bool) - if true this requires that the local node is running in service node
  mode (note that it is *not* required that the local SN be *active*).

Authentication level components are cumulative: for example, a category with Basic auth +
ServiceNode=true + LocalServiceNode=true would only be access if all three conditions are met.

The authentication mechanism works in two ways: defaults based on configuration, and explicit
logins.

Configuration defaults allows controlling the default access for an incoming connection based on its
remote address.  Typically this is used to allow connections from localhost (or a unix domain
socket) to automatically be an Admin connection without requiring explicit authentication.  This
also allows configuration of how public connections should be treated: for example, an oxend running
as a public RPC server would do so by granting Basic access to all incoming connections.

Explicit logins allow the daemon to specify username/passwords with mapping to Basic or Admin
authentication levels.

Thus, for example, a daemon could be configured to be allow Basic remote access with authentication
(i.e. requiring a username/password login given out to people who should be able to access).

For example, in oxend the categories described above have authentication levels of:

- `system` - Admin
- `sn` - ServiceNode
- `blink` - LocalServiceNode
- `blockchain` - Basic

### Service Node authentication

In order to handle ServiceNode authentication, OxenMQ uses an Allow callback invoked during
connection to determine both whether to allow the connection, and to determine whether the incoming
connection is an active service node.

Note that this status persists for the life of the connection (i.e. it is not rechecked on each
command invocation).  If you require stronger protection against being called by
decommissioned/deregistered service nodes from a connection established when the SN was active then
the callback itself will need to verify when invoked.

## Command aliases

Command aliases can be specified.  For example, an alias `a.b` -> `c.d` will rewrite any incoming
`a.b` command to `c.d` before handling it.  These are applied *before* any authentication is
performed.

The main purpose here is for backwards compatibility either for renaming category or commands, or
for changing command access levels by moving it from one category to another.  It's recommended that
such aliases be used only temporarily for version transitions.

## Threads

OxenMQ operates a pool of worker threads to handle jobs.  The simplest use just allocates new jobs
to a free worker thread, and we have a "general threads" value to configure how many such threads
are available.

You may, however, also reserve a minimum number of workers per command category.  For example, you
could reserve 1 thread for the `sys` category and 2 for the `qnet` category plus 8 general threads.
The general threads will be used most of the time for any categories (including `sys` and `qnet`),
but there will always be at least 1/2 worker threads either currently working on or available for
incoming `system`/`sn` commands.  General thread gets used first; only if all general threads are
currently busy *and* a category has unused reserved threads will an additional thread be used.

Note that these actual reserved threads are not exclusive: reserving M of N total threads for a
category simply ensures that no more than (N-M) threads are being used for other categories at any
given time, but the actual jobs may run on any worker thread.

As mentioned above, OxenMQ tries to avoid exceeding the configured general threads value (G)
whenever possible: the only time we will dispatch a job to a worker thread when we have >= G threads
already running is when a new command arrives, the category reserves M threads, and the thread pool
is currently processing fewer than M jobs for that category.

Some examples: assume A and B are commands that take sufficiently long to run that we receive all
commands before the first job is finished.  Suppose that A's category reserves 2 threads, B's
category has no reserved threads, and we have 4 general threads configured.

Example 1: commands arrive in order AABBBB.  This will not exceed 4 threads: when the third B
arrives there are already 4 jobs running so it gets queued.

Example 2: commands arrive in order AABBAA.  This also won't exceed 4 threads: when the third A
arrives there are already 4 jobs running and two of them are already A jobs, so there are no
remaining slots for A jobs.

Example 3: BBBAAA.  This won't exceed 5 threads: the first four get started normally.  When the
second A arrives there are 4 threads running, but only 1 of them is an A thus there is still a free
slot for A jobs so we start the second A on a fifth thread.  The third A, however, has no A jobs
available so gets queued.

Example 4: BBBBBBAAA.  At most 6 jobs.  The 5th and 6th B's get queued (all general workers are
busy).  The first and second get started (there are two unused reserved A slots), the third one gets
queued.  The 5th and 6th B's are already interesting on their own: they won't be started until there
are only three active jobs; the third A won't be started until *either* there are only three active
jobs, or one of the other A's finish.

Thus the general thread count should be regarded as the "normal" thread limit and reserved threads
allow an extra burst of thread activity *only if* all general threads are busy with other categories
when a command with reserve threads arrived.

## Internal batch jobs

A common pattern is one where a single thread suddenly has some work that can be be parallelized.
You could employ some blocking, locking, mutex + condition variable monstrosity, but you shouldn't.

Instead OxenMQ provides a mechanism for this by allowing you to submit a batch of jobs with a
completion callback.  All jobs will be queued and, when the last one finishes, the finalization
callback will be queued to continue with the task.

These batch jobs are quite different from ordinary network commands, as described above: they have
no authentication and can only be submitted by the program itself to its own worker threads.  They
share worker threads with all other commands, as described above, but have their own separate
reserved thread value (for all intents and purposes this works just like a category reserved count).

From the caller point of view this requires splitting the logic into two parts, a "Before" that sets
up the batch, a "Job" that does the work (multiple times), and an "After" that continues once all
jobs are finished.

For example, the following example shows how you might use it to convert from input values (0 to 49)
to some other output value:

```C++
double do_my_task(int input) {
    if (input % 10 == 7)
        throw std::domain_error("I don't do '7s, sorry");
    if (input == 1)
        return 5.0;
    return 3.0 * input;
}

void continue_big_task(std::vector<oxenmq::job_result<double>> results) {
    double sum = 0;
    for (auto& r : results) {
        try {
            sum += r.get();
        } catch (const std::exception& e) {
            std::cout << "Oh noes! " << e.what() << "\n";
        }
    }
    std::cout << "All done, sum = " << sum << "\n";

    // Output:
    // Oh noes! I don't do '7s, sorry
    // Oh noes! I don't do '7s, sorry
    // Oh noes! I don't do '7s, sorry
    // All done, sum = 1337
}

void start_big_task() {
    size_t num_jobs = 32;

    oxenmq::Batch<double /*return type*/> batch;
    batch.reserve(num_jobs);

    for (size_t i = 0; i < num_jobs; i++)
        batch.add_job([i]() { return do_my_task(i); });

    batch.completion(&continue_big_task);

    lmq.batch(std::move(batch));
    // ... to be continued in `continue_big_task` after all the jobs finish

    // Can do other things here, but note that continue_big_task could run
    // *before* anything else here finishes.
}
```

This code deliberately does not support blocking to wait for the tasks to finish: if you want such a
poor design (which is a recipe for deadlocks: imagine jobs that queuing other jobs that can end up
exhausting the worker threads with waiting jobs) then you can implement it yourself; OxenMQ isn't
going to help you hurt yourself like that.

### Single-job queuing

As a shortcut there is a `lmq.job(...)` method that schedules a single task (with no return value)
in the batch job queue.  This is useful when some event requires triggering some other event, but
you don't need to wait for or collect its result.  (Internally this is just a convenience method
around creating a single-job, no-completion Batch job).

You generally do *not* want to use single (or batched) jobs for operations that can block
indefinitely (such as network operations) because each such job waiting on some external condition
ties up a thread.  You are better off in such a case using your own asynchronous operations and
either using your own thread or a periodic timer (see below) to shepherd those operations.

## Timers

OxenMQ supports scheduling periodic tasks via the `add_timer()` function.  These timers have an
interval and are scheduled as (single-job) batches when the timer fires.  They also support
"squelching" (enabled by default) that supresses the job being scheduled if a previously scheduled
job is already scheduled or running.
