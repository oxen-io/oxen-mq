# LokiMQ - zeromq-based message passing for Loki projects

This C++14 library contains an abstraction layer around ZeroMQ to support integration with Loki
authentication and message passing.  It is designed to be usable as the underlying communication
mechanism of SN-to-SN communication ("quorumnet"), the RPC interface used by wallets and local
daemon commands, communication channels between lokid and auxiliary services (storage server,
lokinet), and also provides a local multithreaded job scheduling within a process.

It is not required to use this library to interact with loki components as a client: this is mainly
intended to abstract away much of the server-side handling.

All messages are encrypted (using x25519).

This library makes minimal use of mutexes, and none in the hot paths of the code, instead mostly
relying on ZMQ sockets for synchronization; for more information on this (and why this is generally
much better performing and more scalable) see the ZMQ guide documentation on the topic.

## Basic message structure

LokiMQ messages consist of 1+ part messages where the first part is a string command and remaining
parts are command-specific data.

The command string is one of two types:

`basic` - for basic commands such as authentication (`login`) handled by LokiMQ itself.  These
commands may not contain a `.`, and are reserved for LokiMQ itself.

`category.command` - for commands registered by the LokiMQ caller (e.g. lokid).  Here `category`
must be at least one character not containing a `.` and `command` may be anything.  These categories
and commands are registered according to general function and authentication level (more on this
below).  For example, for lokid categories are:

- `system` - is for RPC commands related to the system administration such as mining, getting
  sensitive statistics, accessing SN private keys, remote shutdown, etc.
- `sn` - is for SN-to-SN communication such as blink quorum and uptime proof obligation votes.
- `blink` - is for public blink commands (i.e. blink submission) and is only provided by nodes
  running as service nodes.
- `blockchain` - is for remote blockchain access such as retrieving blocks and transactions as well
  as subscribing to updates for new blocks, transactions, and service node states.

## Command invocation

The application registers categories and registers commands within these categories with callbacks.
The callbacks are passed a LokiMQ::Message object from which the message (plus various connection
information) can be obtained.  There is no structure imposed at all on the data passed in subsequent
message parts: it is up to the command itself to deserialize however it wishes (e.g. JSON,
bt-encoded, or any other encoding).

The Message object also provides methods for replying to the caller.  Simple replies queue a reply
if the client is still connected.  Replies to service nodes can also be "strong" replies: when
replying to a SN that has closed connection with a strong reply we will attempt to reestablish a
connection to deliver the message.  In order for this to work the LokiMQ caller must provide a
lookup function to retrieve the remote address given a SN x25519 pubkey.

### Callbacks

Invoked command functions are always invoked with exactly one arguments: a non-const LokiMQ::Message
reference from which the connection info, LokiMQ object, and message data can be obtained.  If you
need some extra state data (for example, a reference to some high level object) the LokiMQ object
has an opaque public `void* data` member intended for exactly this purpose.

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
also allows configuration of how public connections should be treated: for example, a lokid running
as a public RPC server would do so by granting Basic access to all incoming connections.

Explicit logins allow the daemon to specify username/passwords with mapping to Basic or Admin
authentication levels.

Thus, for example, a daemon could be configured to be allow Basic remote access with authentication
(i.e. requiring a username/password login given out to people who should be able to access).

For example, in lokid the categories described above have authentication levels of:

- `system` - Admin
- `sn` - ServiceNode
- `blink` - LocalServiceNode
- `blockchain` - Basic

### Service Node authentication

In order to handle ServiceNode authentication, LokiMQ uses an Allow callback invoked during
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

LokiMQ operates a pool of worker threads to handle jobs.  The simplest use just allocates new jobs
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

As mentioned above, LokiMQ tries to avoid exceeding the configured general threads value (G)
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

Exmaple 4: BBBBBBAAA.  At most 6 jobs.  The 5th and 6th B's get queued (all general workers are
busy).  The first and second get started (there are two unused reserved A slots), the third one gets
queued.  The 5th and 6th B's are already interesting on their own: they won't be started until there
are only three active jobs; the third A won't be started until *either* there are only three active
jobs, or one of the other A's finish.

Thus the general thread count should be regarded as the "normal" thread limit and reserved threads
allow an extra burst of thread activity *only if* all general threads are busy with other categories
when a command with reserve threads arrived.

## Internal job queuing

This library supports queuing internal jobs (internally these are in the "" (empty string) category,
which is not externally accessible).  These jobs are quite different from ordinary jobs: they have
no authentication and can only be submitted by the program itself to its own worker threads.  They
have either no second message part, or else one single message part consists of an opaque void
pointer value.  This pointer is passed by value to the registered function, which must take exactly
one `void *` argument.

It is entirely the responsibility of the caller and callee to deal with the `void *` argument,
including construction/destruction/etc.  This is very low level but allow the most flexibility.  For
example, a caller might do something like:

```C++
// Set up a function that takes ownership:
void hello1(void *data) {
    auto* str = static_cast<std::string*>(data);
    std::cout << "Hello1 " << *str << "\n";
    delete str;
}
LokiMQ::register_task("hello1", &hello1);

// Another function that doesn't take ownership (and handles nullptr):
void hello2(void *data) { // 
    std::cout << "Hello2 " <<
        (data ? *static_cast<std::string*>(data) : "anonymous") <<
        "\n";
}
LokiMQ::register_task("hello2", &hello2);


// Later, in the calling code:
const static std::string there{"there"};
void myfunc() {
    // ...
    lmq.job(&hello1, new std::string{"world"}); // Give up ownership of the pointer
    lmq.job(&hello2, &there); // Passing an externally valid pointer
    // But don't do this:
    //std::string world{"world"};
    //lmq.job(&hello2, &world); // Bad: `world` will probably be destroyed
                                // before the callback gets invoked
}
```

### Dealing with synchronization of jobs

A common pattern is one where a single thread suddenly has some work that can be be parallelized.
We can easily queue all the jobs into the worker thread pool (see above), but the issue then is how
to continue when the work is done.  You could (but shouldn't) employ some blocking, locking, mutex +
condition variable monstrosity, but you shouldn't.

Instead LokiMQ provides a mechanism for this by allowing you to submit a batch of jobs with a
completion callback.  All jobs will be queued and, when the last one finishes, the finalization
callback will be queued to continue with the task.

From the caller point of view this requires splitting the logic into two parts, a "Before" that sets
up the batch, a "Job" that does the work (multiple times), and an "After" that continues once all
jobs are finished.

For example, the following example shows how you might use it to convert from input values (0 to 49)
to some other output value:

```C++
struct task_data { int input; double result; };

// Called for each job.
void do_my_task(void* in) {
    auto& x = *static_cast<task_data*>(in);
    x.result = 42.0 * x.input; // Job
}

void start_big_task() {
    // ... Before code ...

    auto* results = new std::vector<task_data>{50};

    lokimq::Batch batch;
    for (size_t i = 0; i < results->size(); i++) {
        auto* r = (*result)[i];
        r->input = i;
        batch.add_job(&do_my_task, r);
    }
    lmq.job(batch, &continue_big_task, results);
    // ... to be continued in `continue_big_task` after all the jobs finish
}

// This will be called once all the `do_my_task` calls have completed.  (Note that we could be in
// a different thread from the one `start_big_task()` was running in).
void continue_big_task(void* rptr) {
    // Put into a unique_ptr to deal with ownership
    std::unique_ptr<std::vector<task_data>> results{static_cast<std::vector<int>*>(rptr)};
    double sum = 0;
    for (auto &r : results) sum += r;
    std::cout << "All done, sum = " << sum << "\n";
}
```

This code deliberately does not support blocking to wait for the tasks to finish: if you want such a
bad design you can implement it yourself; LokiMQ isn't going to help you hurt yourself.


