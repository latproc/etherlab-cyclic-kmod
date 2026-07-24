# Clockwork / EtherLab Generic Kernel EtherCAT Transport
## Detailed implementation plan for Codex CLI

**Target repository:** `https://github.com/latproc/clockwork`  
**Starting branch:** `prod-experimental-mqtt-fix`  
**Primary application:** `iod` / `iod_sdo`  
**Objective:** Move the deterministic EtherCAT cyclic exchange out of the Clockwork user-space process and into a generic, runtime-configurable Linux kernel module, while keeping machine knowledge, device matching, Beckhoff XML parsing, PDO policy, and application semantics in Clockwork user space.

---

# 1. Instructions to the Codex instance

This is not a request to immediately rewrite the EtherCAT subsystem.

Before making substantial changes:

1. Read the existing EtherCAT implementation in `iod` completely enough to understand:
   - master acquisition and release;
   - slave discovery;
   - `ECModule` construction;
   - XML-driven PDO replacement;
   - PDO configuration;
   - domain registration;
   - process image access;
   - cyclic receive/process/queue/send sequence;
   - distributed-clock handling if enabled;
   - runtime SDO handling;
   - startup ordering;
   - thread scheduling and CPU affinity;
   - shutdown and restart behaviour.

2. Inspect the exact EtherLab/IgH master version installed or targeted by this project.
   Do not assume that an API found in another EtherLab release is present in the target version.

3. Read the installed EtherLab kernel API header (`ecrt.h`) and the corresponding EtherLab source where necessary.

4. Do not invent an ABI until the required EtherLab operations have been confirmed to work from a separate kernel application module.

5. Build and test each stage independently.
   The first kernel-module experiments must not depend on Clockwork.

6. Preserve the existing IOD EtherCAT implementation behind a build-time or runtime fallback until the new path is proven.

7. Prefer small commits that leave a runnable or testable state.

8. Treat real-time behaviour carefully:
   - no memory allocation in the cyclic path;
   - no blocking mailbox operations in the cyclic path;
   - no logging in the normal per-cycle path;
   - no user-space calls from the cyclic path;
   - carefully define locking and memory ordering;
   - never hold configuration locks from the real-time thread longer than necessary.

9. Do not put machine-specific knowledge in the kernel module.

10. Do not put Beckhoff XML parsing in the kernel module.

11. Do not hard-code Summa/Estun servo identities or PDO layouts in the kernel module.

The intended kernel module is a **generic EtherCAT transport and configuration mechanism**, not a Clockwork device driver.

---

# 2. Existing system: important behaviour that must be preserved

The present startup path is approximately:

```text
iod.sh
    |
    +-- set CPU governor to performance
    |
    +-- wait until `ethercat master` reports Link UP
    |
    +-- apply machine-specific IRQ affinity policy
    |
    +-- scan `ethercat slaves`
    |
    +-- find all slaves whose displayed name matches:
    |       Summa ED3L ServoDrive
    |
    +-- run sdo.sh against each discovered servo position
    |       |
    |       +-- rewrite 0x1600 RxPDO mapping
    |       +-- rewrite 0x1A00 TxPDO mapping
    |       +-- assign PDOs through 0x1C12 / 0x1C13
    |       +-- set mode of operation 0x6060 = 3
    |
    +-- start iod_sdo
            |
            +-- scan EtherCAT bus
            +-- create ECModule list
            +-- match Clockwork MODULE definitions
            +-- optionally parse customised Beckhoff XML
            +-- construct replacement PDO/sync configuration
            +-- configure EtherLab slaves
            +-- register PDO entries/domain
            +-- activate master
            +-- run cyclic EtherCAT processing
            +-- service runtime SDOEntry operations
```

This behaviour must not disappear accidentally during refactoring.

---

# 3. Current XML PDO configuration behaviour

Clockwork currently has an important capability that must remain in user space.

A Clockwork `MODULE` can specify values including:

```text
position
config_file
alternate_sync_manager
ProductCode
RevisionNo
```

The current startup code:

1. Scans the real bus.
2. Builds a positional slave list.
3. Looks through Clockwork `MODULE` instances.
4. For modules with a `config_file`:
   - identifies the actual product code and revision from the scanned bus when they were not explicitly supplied;
   - parses the selected Beckhoff/ESI-style XML;
   - finds the matching device configuration;
   - extracts Sync Manager, PDO and PDO-entry information;
   - creates/replaces the corresponding `ECModule`.

The resulting `ECModule` carries data equivalent to:

```text
slave identity:
    alias
    position
    vendor_id
    product_code
    revision

PDO configuration:
    syncs
    sync_count
    PDOs
    PDO entries
    entry details
    num_entries
```

The kernel-module architecture must preserve this responsibility in Clockwork.

The intended split is:

```text
Beckhoff XML
    |
Clockwork XML parser
    |
Clockwork decides desired PDO layout
    |
serialised generic configuration
    |
kernel module
    |
EtherLab ecrt_slave_config_pdos(...)
```

The kernel module must not know that the data originated from XML.

---

# 4. Current servo PDO setup behaviour

The current `sdo.sh` changes the physical ED3L servo's CoE PDO mapping before IOD starts.

For each detected servo it performs an ordered sequence equivalent to:

## 4.1 Disable / clear RxPDO assignment

```text
0x1C12:00 = 0
0x1C12:01 = 0
```

## 4.2 Clear RxPDO mapping object 0x1600

```text
0x1600:00 = 0

0x1600:01 = 0
0x1600:02 = 0
0x1600:03 = 0
0x1600:04 = 0
0x1600:05 = 0
0x1600:06 = 0
```

## 4.3 Clear TxPDO mapping object 0x1A00

```text
0x1A00:00 = 0

0x1A00:01 = 0
0x1A00:02 = 0
0x1A00:03 = 0
0x1A00:04 = 0
0x1A00:05 = 0
```

## 4.4 Build RxPDO 0x1600

```text
0x1600:01 = 0x60400010   # Control word, 16 bit
0x1600:02 = 0x60FF0020   # Target velocity, 32 bit
0x1600:03 = 0x60830020   # Profile acceleration, 32 bit
0x1600:04 = 0x60840020   # Profile deceleration, 32 bit
0x1600:05 = 0x60E00010   # 16 bit
0x1600:06 = 0x60E10010   # 16 bit
0x1600:00 = 6
```

## 4.5 Build TxPDO 0x1A00

```text
0x1A00:01 = 0x60410010   # Status word, 16 bit
0x1A00:02 = 0x606C0020   # Actual velocity, 32 bit
0x1A00:03 = 0x60770010   # Actual torque, 16 bit
0x1A00:04 = 0x603F0010   # Error code, 16 bit
0x1A00:00 = 4
```

## 4.6 Assign PDOs to Sync Managers

```text
0x1C12:01 = 0x1600
0x1C13:01 = 0x1A00

0x1C12:00 = 1
0x1C13:00 = 1
```

## 4.7 Select operating mode

```text
0x6060:00 = 3
```

This sequence is **ordered** and should not be treated as an unordered set of defaults.

In the eventual architecture, this sequence should become a generic ordered list of typed pre-activation SDO writes supplied by user space.

The kernel must not contain the ED3L recipe.

---

# 5. Target architecture

The intended architecture is:

```text
+-------------------------------------------------------------+
| CLOCKWORK / IOD                                             |
|                                                             |
| Owns:                                                       |
|   machine model                                             |
|   MODULE definitions                                        |
|   slave/device matching                                     |
|   Beckhoff XML / ESI parsing                                |
|   alternate_sync_manager selection                          |
|   choice of PDO layout                                      |
|   choice of startup SDO recipe                              |
|   interpretation of process values                          |
|   application state and logic                               |
|   SDOEntry semantics                                        |
+------------------------------+------------------------------+
                               |
                               | ioctl/read/write/mmap
                               | or another deliberately chosen ABI
                               |
+------------------------------v------------------------------+
| GENERIC CLOCKWORK ETHERCAT KERNEL MODULE                    |
|                                                             |
| Owns:                                                       |
|   EtherLab master application ownership                     |
|   scanned bus information                                   |
|   EtherLab slave_config objects                             |
|   typed ordered setup SDO transactions                      |
|   sync/PDO/PDO-entry application to EtherLab                |
|   domain creation and PDO registration                      |
|   process-image memory                                      |
|   actual cyclic receive/process/queue/send                  |
|   cycle statistics and bus status                           |
|   eventual asynchronous runtime SDO request execution       |
+------------------------------+------------------------------+
                               |
                               v
                       EtherLab master
                               |
                               v
                         EtherCAT network
```

The kernel module is deliberately "dumb" regarding device meaning.

It must understand EtherCAT mechanisms, not Clockwork machines.

---

# 6. Non-goals for the first implementation

Do **not** initially attempt all of the following at once:

- replace every existing `ECInterface` feature;
- move the Clockwork XML parser into the kernel;
- redesign Clockwork's machine language;
- redesign all SDOEntry semantics;
- create automatic ESI discovery in the kernel;
- hot-reconfigure PDO layouts while the cyclic loop remains active;
- support arbitrary multiple Clockwork processes;
- support multiple independent EtherLab applications sharing one master;
- optimise zero-copy behaviour before correctness is established;
- remove the existing user-space EtherCAT code immediately.

The project should be built in testable increments.

---

# 7. Phase 0 — repository and EtherLab investigation

## Goal

Produce a written map of the current implementation before changing architecture.

## Tasks

Inspect at least:

```text
iod/src/ECInterface.h
iod/src/ECInterface.cpp
iod/src/ecat_thread.h
iod/src/ecat_thread.cpp
iod/src/iod.cpp
iod/src/EtherCATSetup.*
iod/src/ethercat_xml_parser.*
iod/src/process_data.*
iod/src/SDOEntry.*
iod/CMakeLists.txt
top-level build files
```

Search for every use of:

```text
ecrt_request_master
ecrt_release_master
ecrt_master_create_domain
ecrt_master_slave_config
ecrt_slave_config_pdos
ecrt_slave_config_reg_pdo_entry
ecrt_domain_reg_pdo_entry_list
ecrt_master_activate
ecrt_master_deactivate
ecrt_domain_data

ecrt_master_receive
ecrt_domain_process
ecrt_domain_queue
ecrt_master_send

ecrt_master_state
ecrt_domain_state
ecrt_slave_config_state

ecrt_slave_config_create_sdo_request
ecrt_sdo_request_*

ecrt_master_application_time
ecrt_master_sync_slave_clocks
ecrt_master_reference_clock_time
```

Document:

1. Which thread currently calls each operation.
2. Which operations happen before activation.
3. Which operations happen every cycle.
4. How domain offsets and bit positions are stored.
5. How `IOComponent` reads and writes those offsets.
6. How machine readiness interacts with EtherCAT startup.
7. What happens if the link drops.
8. What happens if a slave goes offline.
9. What happens when IOD exits or crashes.
10. Whether IOD currently reclaims/reinitialises the master on restart.

## EtherLab investigation

Determine:

```text
EtherLab/IgH version
kernel version
PREEMPT_RT status
EtherLab install prefix
location of ecrt.h
location/export status of EtherLab kernel symbols
```

Confirm whether a separately built out-of-tree application module can link against and call the required exported `ecrt_*` symbols.

This is a hard prerequisite.

## Deliverable

Create:

```text
docs/ethercat-kernel/current-architecture.md
```

Do not proceed to large IOD changes until this document exists.

---


# 7A. Kernel safety, lifetime, and memory-management requirements

The kernel module must be designed conservatively. A failure in this code can crash or corrupt the entire machine, not merely terminate IOD.

Correctness, bounded resource use, and clean lifetime management take priority over performance optimisations.

## Mandatory design rule: explicit ownership

For every dynamically allocated object, document:

```text
who allocates it
who owns it
when ownership transfers
who frees it
what happens on partial failure
what happens during configuration reset
what happens during deactivate
what happens during file close
what happens during module unload
```

This applies at least to:

```text
master references
domains
slave configuration metadata
Sync Manager arrays
PDO arrays
PDO-entry arrays
entry registration metadata
setup SDO transaction buffers
runtime SDO request objects
process-image shadow buffers
mmap backing storage
configuration generations
kernel threads
timers
work queues
character-device state
per-open-file state
```

Do not rely on comments such as "freed later."
The ownership must be mechanically clear from the code structure.

Prefer a small number of top-level owner objects with deterministic teardown.

For example:

```text
cw_ec_device
    |
    +-- active configuration
    |       |
    |       +-- slaves
    |       +-- syncs
    |       +-- PDOs
    |       +-- entries
    |       +-- setup SDOs
    |       +-- process image
    |
    +-- cyclic thread
    +-- character device
```

A configuration object should own all allocations created for that configuration.

This allows one cleanup function to tear down a partially or fully constructed configuration.

---

## Mandatory rule: every construction path must have a failure unwind path

Kernel code must assume that any allocation or EtherLab call can fail.

Do not write code shaped like:

```c
a = kmalloc(...);
b = kmalloc(...);
c = kmalloc(...);

if (!c)
    return -ENOMEM;
```

because that leaks `a` and `b`.

Use a structured unwind pattern or top-level cleanup helper.

Example pattern:

```c
cfg = kzalloc(...);
if (!cfg)
    return -ENOMEM;

cfg->entries = kcalloc(...);
if (!cfg->entries) {
    ret = -ENOMEM;
    goto fail;
}

cfg->syncs = kcalloc(...);
if (!cfg->syncs) {
    ret = -ENOMEM;
    goto fail;
}

/* more setup */

return 0;

fail:
    cw_ec_config_destroy(cfg);
    return ret;
```

The same cleanup function should be safe against partially initialised objects.

Use `kzalloc`/`kcalloc` where zero-initialisation makes teardown safer.

---

## Mandatory rule: cleanup functions must be idempotent where practical

Functions such as:

```text
cw_ec_stop_cycle()
cw_ec_deactivate()
cw_ec_config_destroy()
cw_ec_release_master()
cw_ec_device_cleanup()
```

should tolerate partially initialised state.

Prefer:

```c
if (ptr) {
    kfree(ptr);
    ptr = NULL;
}
```

and explicit state transitions.

Avoid teardown logic that depends on fragile assumptions about how far initialisation progressed.

---

## Mandatory rule: no unchecked user-space input

All ioctl or write-based configuration data is untrusted, even when the caller is normally IOD.

Validate before allocation and before calling EtherLab.

Check at least:

```text
struct version
struct size
count fields
array lengths
integer overflow
multiplication overflow
addition overflow
maximum slave count
maximum Sync Manager count
maximum PDO count
maximum entry count
maximum SDO payload size
valid enum values
valid bit lengths
valid indices/subindices
valid references between IDs
duplicate IDs
configuration state
```

Use kernel overflow helpers where available, such as:

```text
check_mul_overflow
check_add_overflow
array_size
struct_size
```

Never calculate an allocation size from user input with unchecked arithmetic.

Do not dereference user pointers directly.

Use the appropriate:

```text
copy_from_user
copy_to_user
```

interfaces and verify return values.

Avoid embedding raw user pointers in persistent kernel objects.

---

## Mandatory rule: define hard resource limits

The kernel module must not allow a malformed or accidental configuration to consume arbitrary kernel memory.

Define conservative configurable or compile-time limits for the initial implementation, for example:

```text
maximum slaves
maximum Sync Managers per slave
maximum PDOs
maximum PDO entries
maximum setup SDO operations
maximum individual SDO payload
maximum total setup-SDO memory
maximum process-image size
maximum runtime SDO handles
```

The exact values should be based on realistic Clockwork machines, not arbitrary tiny values.

Reject configurations that exceed the limits with a clear error.

Limits may be increased later after measurement.

---

## Mandatory rule: configuration should be transactional

Do not modify the active configuration incrementally while processing a stream of ioctls.

Prefer:

```text
active_config
pending_config
```

Flow:

```text
CONFIG_BEGIN
      |
allocate pending_config
      |
populate and validate pending_config
      |
CONFIG_VALIDATE
      |
apply EtherLab configuration
      |
CONFIG_COMMIT
      |
atomically make it active
```

On any failure:

```text
destroy pending_config
leave active_config unchanged
```

For the first implementation, configuration should only be committed while the cyclic engine is stopped.

This greatly reduces use-after-free and partial-configuration risks.

---

## Mandatory rule: no freeing memory visible to the cyclic thread

The cyclic thread must never access configuration or process-image memory that can concurrently be freed.

Use a simple lifecycle first:

```text
STOPPED
    |
configure
    |
READY
    |
start thread
    |
RUNNING
    |
stop thread and wait for confirmed exit
    |
free configuration
```

Do not attempt clever lock-free configuration replacement initially.

Before freeing anything used by the cyclic thread:

1. signal the thread to stop;
2. wake it if necessary;
3. wait synchronously for thread termination;
4. ensure timers/work items are cancelled;
5. only then free process/configuration memory.

Use kernel primitives appropriate to the implementation, such as:

```text
kthread_stop
del_timer_sync
hrtimer_cancel
cancel_work_sync
cancel_delayed_work_sync
```

as applicable.

The exact functions depend on the selected timing architecture.

---

## Mandatory rule: module unload must be boring

`rmmod` must never race with an active cyclic thread, open device operation, pending SDO work, timer callback, work queue, or mmap lifetime.

The module exit path must have an explicit order, for example:

```text
prevent new opens / operations
        |
mark device shutting down
        |
stop cyclic engine
        |
cancel timers
        |
cancel/flush workers
        |
deactivate/release EtherLab resources
        |
destroy pending and active configurations
        |
destroy device nodes
        |
free module-global allocations
```

Codex must verify the real required order against the implemented objects.

Do not allow module unload while references remain unless the kernel's module reference counting guarantees it cannot occur.

Ensure file operations maintain appropriate module references.

---

## Mandatory rule: handle open file descriptors carefully

Define per-file state deliberately.

Questions that must be answered:

```text
Can more than one process open the device?
Which fd owns configuration?
Which fd owns activation?
Can diagnostic clients open read-only?
What happens if the controller fd exits unexpectedly?
Can an mmap survive close?
What prevents configuration objects from being freed while mapped?
```

For the initial prototype, the safest model may be:

```text
one exclusive control open
optional separate read-only diagnostic interface later
```

Do not implement multi-client control until it is needed.

---

## Mandatory rule: mmap lifetime must be proven

Memory exposed through `mmap()` has particularly dangerous lifetime rules.

Before using mmap, Codex must understand and document:

```text
allocation method
page alignment
mapping method
VMA open callback
VMA close callback
reference counting
what happens when controller process exits
what happens when configuration resets
what happens when module unload is requested
```

The kernel must not free pages while user space still has a valid VMA referencing them.

A configuration generation should not be destroyed merely because the controlling fd closes if an mmap reference still exists, unless mappings are explicitly invalidated using a correct kernel mechanism.

If mmap lifetime becomes unnecessarily complex for the first prototype, use `read()`/`write()` or another simpler copying API first, prove the architecture, then add mmap.

Safety is more important than premature zero-copy performance.

---

## Mandatory rule: avoid use-after-free through asynchronous work

Runtime SDO handling, timers, and worker threads can easily retain pointers after an object is destroyed.

Every asynchronous object must have a defined cancellation and completion mechanism.

Never:

```text
queue work containing pointer to config
free config
```

without first synchronously cancelling/flushing that work.

Similarly, an SDO request must not reference a slave configuration that has already been destroyed or deactivated.

---

## Mandatory rule: locking design must be documented

Before implementing substantial concurrency, document:

```text
which data is immutable while RUNNING
which data is cyclic-thread-only
which data is user-context-only
which data is shared
which lock protects each shared field
lock acquisition order
whether a lock may sleep
whether the cyclic thread may take it
```

Avoid mutexes in the real-time cyclic path where possible.

Prefer immutable configuration during RUNNING.

A simple model is:

```text
configuration:
    immutable while RUNNING

process input/output shadows:
    shared through sequence counters or bounded spinlock strategy

status counters:
    atomics or carefully protected snapshots

control operations:
    mutex in process context
```

Do not use spinlocks around operations that may sleep.

Do not call blocking EtherLab operations while holding spinlocks.

---

## Mandatory rule: use kernel sanitising/debug facilities during development

Where supported by the target test system, build and test development kernels/modules with:

```text
KASAN
KFENCE
KCSAN where useful
kmemleak
CONFIG_DEBUG_LIST
CONFIG_DEBUG_SLAB or slab debugging
CONFIG_DEBUG_VM
lockdep
CONFIG_PROVE_LOCKING
```

Not all need to be enabled simultaneously on production hardware.

At minimum, provide a development test procedure for:

### kmemleak

Repeatedly:

```text
load module
open
scan
configure
activate
deactivate
close
unload
```

and check for leaks.

### KASAN/KFENCE

Exercise malformed configurations, repeated teardown, and failure injection.

### lockdep

Exercise concurrent status queries, activation/deactivation and shutdown.

Do not treat a clean normal run as sufficient evidence of memory safety.

---

## Mandatory rule: fault-injection testing

Where feasible, deliberately exercise failures after each construction stage.

Examples:

```text
fail first allocation
fail second allocation
fail entry-array allocation
fail SDO-buffer allocation
fail master request
fail domain creation
fail slave config
fail PDO config
fail registration
fail cyclic thread creation
```

The expected result is always:

```text
clean error returned
no kernel oops
no dangling thread
no leaked allocation
master remains recoverable
module can unload
```

Consider adding development-only fault injection parameters such as:

```text
fail_step=N
```

if kernel fault-injection infrastructure is inconvenient.

Remove or guard these appropriately for production.

---

## Mandatory rule: repeated lifecycle stress test

Create a standalone stress tool/script that performs many repetitions of:

```text
open
get bus
begin config
submit config
commit
activate
run briefly
deactivate
reset config
close
```

Also test repeated:

```text
modprobe
test
rmmod
```

when safe.

Run hundreds or thousands of cycles on a development machine.

Monitor:

```text
dmesg
kmemleak
slab usage
thread count
module reference count
EtherLab master availability
```

No steadily increasing memory usage is acceptable.

---

## Mandatory rule: check every kernel API return value

Do not ignore failures from:

```text
allocations
copy_from_user
copy_to_user
device registration
class/device creation
thread creation
timer setup where applicable
EtherLab calls
configuration calls
activation calls
mmap setup
```

Return useful negative errno values to user space.

Kernel logs should provide enough context to diagnose unexpected failures but should not spam repeatedly.

---

## Mandatory rule: no BUG(), BUG_ON(), or panic-style assertions for recoverable input/runtime errors

Do not convert a bad Clockwork configuration into a kernel crash.

Avoid:

```text
BUG()
BUG_ON()
panic()
```

for conditions that can result from:

```text
bad user input
missing slave
configuration mismatch
SDO failure
link loss
allocation failure
state mismatch
```

Return an error and move to a safe module state.

`WARN_ON` should also be used carefully because production machines should not generate kernel warnings for expected recoverable conditions.

Kernel assertions are not a replacement for validation.

---

## Mandatory rule: protect against stale handles and generations

If the UAPI exposes IDs for:

```text
PDO entries
runtime SDO requests
configuration objects
```

associate them with a configuration generation.

After:

```text
RESET_CONFIG
```

old IDs must not accidentally reference newly allocated objects that reused the same numeric index or memory address.

Use monotonically changing generation identifiers or robust handle tables.

Never expose kernel pointers as handles.

---

## Mandatory rule: explicit reference counting where ownership is shared

Use reference counting only where true shared lifetime exists.

Possible examples:

```text
mmap-backed process image
per-open device object
runtime SDO handles
```

Use kernel mechanisms such as `kref` or `refcount_t` where appropriate.

Do not invent ad-hoc integer reference counters without overflow protection and defined ownership semantics.

Conversely, do not add reference counting everywhere.
Prefer single ownership where possible.

---

## Mandatory rule: test malformed ABI calls

Create a test program that deliberately sends:

```text
unknown ioctl
wrong API version
short struct
oversized struct
zero counts
huge counts
invalid IDs
duplicate IDs
out-of-order calls
configure while running
activate twice
deactivate twice
reset while running
invalid slave position
invalid SDO payload length
invalid mmap offset
invalid mmap length
```

Expected behaviour:

```text
well-defined errno
no kernel warning
no oops
no leak
module remains usable
```

Use syscall fuzzing tools only after the explicit tests are correct.

---

## Mandatory rule: prefer simpler kernel code over clever kernel code

For the first production-capable version:

Prefer:

```text
copying a 1 KB process image
```

over:

```text
complicated zero-copy shared ownership with fragile VMA lifetimes
```

Prefer:

```text
stop -> reconfigure -> restart
```

over:

```text
live RCU replacement of PDO configuration
```

Prefer:

```text
one controller
```

over:

```text
multi-client arbitration
```

Prefer:

```text
bounded arrays and explicit limits
```

over:

```text
unbounded user-supplied object graphs
```

Once the implementation is proven stable, optimisation can be based on measurement.

---

## Kernel-safety acceptance gate

Before any IOD integration begins, the standalone kernel prototype should satisfy all of the following:

1. Repeated module load/unload does not oops.
2. Repeated master acquire/release leaves EtherLab usable.
3. Repeated scan operations do not increase memory use.
4. Invalid ioctls/configurations return errors without warnings or crashes.
5. Partial allocation failure paths have been exercised.
6. Configuration create/destroy stress tests show no leak.
7. Cyclic thread start/stop stress tests leave no threads behind.
8. SDO setup failure does not leave dangling transactions.
9. Module unload is blocked or safely handled while resources are in use.
10. kmemleak or equivalent testing shows no persistent module allocations after teardown.
11. KASAN/KFENCE testing, where available, shows no invalid memory access.
12. lockdep testing, where available, reports no locking violations.
13. The EtherLab master can be used normally after the module has fully released it.

This gate must be passed before making the kernel backend the default path in IOD.


# 8. Phase 1 — prove a minimal external kernel application module

## Goal

Prove that an out-of-tree kernel module can request the EtherLab master and inspect it.

This test must not involve Clockwork.

## Suggested directory

Either inside Clockwork:

```text
kernel/clockwork_ethercat/
```

or, if the build is easier to isolate initially:

```text
experiments/ethercat_kernel/
```

A likely eventual tree:

```text
kernel/
    clockwork_ethercat/
        Makefile
        Kbuild
        cw_ec_module.c
        cw_ec_internal.h
        cw_ec_uapi.h
```

## First test module

The first module should do as little as possible.

On load or through an explicit test action:

1. Request EtherLab master 0.
2. Query master state/information.
3. Log success or failure.
4. Release the master.
5. Unload cleanly.

Do not start a cyclic thread yet.

## Critical questions to answer

- Can the module link against EtherLab exported symbols?
- Can it be built against the running kernel?
- Does EtherLab require a particular include/build path?
- Does loading the test module prevent `ethercat` CLI access?
- When does master exclusivity begin?
- Can the `ethercat` CLI still perform SDO operations before the test module requests the master?
- What happens when the kernel module releases the master?
- Can user-space libethercat request it afterward?
- Can repeated load/unload cycles occur without leaving the master busy?

## Standalone test utility

Add a script:

```text
tools/cw_ec_test_master.sh
```

It should:

1. print `ethercat master`;
2. load the test module;
3. collect `dmesg` lines from the module;
4. unload it;
5. verify `ethercat master` remains usable.

## Acceptance criterion

No IOD changes.

The experiment is successful only if master acquisition/release semantics are clearly understood.

---

# 9. Phase 2 — bus discovery from the kernel module

## Goal

Make the module able to report what it believes the EtherCAT bus looks like.

This must support Clockwork's existing scan-and-match behaviour later.

## Desired information per slave

At minimum:

```c
struct cw_ec_slave_info {
    __u16 position;
    __u16 alias;

    __u32 vendor_id;
    __u32 product_code;
    __u32 revision_number;
    __u32 serial_number;

    __u8 al_state;
    __u8 online;
    __u8 operational;
    __u8 reserved;
};
```

Only include fields the target EtherLab API can reliably provide.

Consider whether slave name should be exposed.

Do not make matching depend on the human-readable name.

Clockwork should eventually match servo devices by identity rules rather than:

```bash
awk '/Summa ED3L ServoDrive/'
```

## Kernel state model

Introduce a module state machine early.

For example:

```c
enum cw_ec_state {
    CW_EC_STATE_DOWN,
    CW_EC_STATE_WAITING_FOR_LINK,
    CW_EC_STATE_SCANNING,
    CW_EC_STATE_BUS_READY,
    CW_EC_STATE_CONFIGURING,
    CW_EC_STATE_READY,
    CW_EC_STATE_RUNNING,
    CW_EC_STATE_ERROR,
};
```

Do not assume these exact values are correct.

Investigate EtherLab's scan lifecycle first.

## User-space ABI test tool

Create a small independent utility:

```text
tools/cw_ec_bus
```

Preferred language: C or C++ with minimal dependencies.

Example desired behaviour:

```text
$ cw_ec_bus
master: 0
state: BUS_READY
link: up
slaves: 7

pos  alias  vendor      product     revision    state
0    0      0x00000002  0x044c2c52  ...
1    0      ...
```

This utility must not use Clockwork classes.

## ABI choice

A character device is a reasonable initial direction:

```text
/dev/cw_ethercat0
```

Potential operations:

```text
CW_EC_IOC_GET_API_VERSION
CW_EC_IOC_GET_MASTER_INFO
CW_EC_IOC_GET_SLAVE_COUNT
CW_EC_IOC_GET_SLAVE_INFO
```

Before freezing this ABI:

- consider 32/64-bit compatibility;
- avoid native pointers in UAPI structs;
- use fixed-width Linux UAPI types;
- include struct sizes/version fields where useful;
- consider future extension;
- validate all user-supplied lengths.

## Acceptance criterion

With IOD stopped:

```text
cw_ec_bus
```

must report a bus topology that can be compared against:

```text
ethercat slaves
ethercat master
```

The reported physical slave order and identity must agree.

---

# 10. Phase 3 — generic pre-activation SDO write mechanism

## Goal

Replace the mechanism used by `sdo.sh`, initially through a standalone test tool.

Do not connect this to IOD yet.

## Important design requirement

Setup SDOs are an **ordered transaction stream**.

They are not equivalent to Clockwork's existing runtime `SDOEntry` objects.

Suggested conceptual structure:

```c
enum cw_ec_sdo_data_type {
    CW_EC_SDO_U8,
    CW_EC_SDO_S8,
    CW_EC_SDO_U16,
    CW_EC_SDO_S16,
    CW_EC_SDO_U32,
    CW_EC_SDO_S32,
    CW_EC_SDO_BYTES,
};

struct cw_ec_setup_sdo {
    __u32 sequence;
    __u16 position;
    __u16 index;
    __u8 subindex;
    __u8 type;
    __u16 data_len;
    /* data supplied separately or in a bounded payload */
};
```

Do not assume a 32-bit value is sufficient forever.

Support arbitrary byte payloads eventually if the EtherLab API requires it.

## Configuration transaction

Prefer a transaction model:

```text
CONFIG_BEGIN
    |
    +-- ADD_SETUP_SDO
    +-- ADD_SETUP_SDO
    +-- ...
    |
CONFIG_VALIDATE
    |
CONFIG_APPLY_SETUP_SDOS
```

On failure:

- report which transaction failed;
- report the SDO index/subindex;
- preserve the EtherLab/abort error where available;
- do not silently continue unless explicitly requested.

The old Bash script currently logs failure and moves on at the per-servo level.
The new implementation should make error policy explicit.

## Standalone conversion test

Create:

```text
tools/cw_ec_sdo
```

It should be capable of performing operations such as:

```text
cw_ec_sdo write --position 5 --type u8  0x1600 0 0
cw_ec_sdo write --position 5 --type u32 0x1600 1 0x60400010
```

Then add a recipe mechanism:

```text
tools/recipes/ed3l_velocity_pdo.json
```

or another easy-to-parse temporary format.

The standalone tool should:

1. discover selected servo(s);
2. apply the same ordered writes currently in `sdo.sh`;
3. report each result;
4. allow verification with existing `ethercat upload` commands.

This tool is for proving the kernel ABI.
It is not necessarily the permanent Clockwork configuration format.

## Acceptance criterion

With Clockwork stopped, the new kernel interface plus test tool must reproduce the effect of the existing `sdo.sh`.

Verify relevant objects by reading back at least:

```text
0x1600
0x1A00
0x1C12
0x1C13
0x6060
```

Compare old and new results.

**Immediately after this acceptance criterion is met, perform the PDO Setup Mechanism Decision Gate in Section 10A.** Do not finalise the Phase 4 configuration ABI or assume that ED3L production operation will use raw SDO mapping writes until the explicit-SDO and declarative `ecrt_slave_config_pdos()` paths have been tested against each other.

---


# 10A. PDO setup mechanism decision gate — test standard EtherLab configuration before depending on raw SDO recipes

The ED3L servo currently has a known-good explicit SDO sequence in `sdo.sh` that rewrites:

```text
0x1600   RxPDO mapping
0x1A00   TxPDO mapping
0x1C12   RxPDO Sync Manager assignment
0x1C13   TxPDO Sync Manager assignment
0x6060   Mode of operation
```

EtherLab normally expects PDO assignment and PDO mapping to be described declaratively through its slave PDO configuration API, such as `ecrt_slave_config_pdos()`. However, the ED3L drive may have device-specific ordering or state requirements that make the explicit SDO sequence necessary.

Do not assume either behaviour.

This must be resolved experimentally **before the kernel ABI is made dependent on one approach**.

## Where this test occurs in the development plan

Perform this test after:

```text
Phase 1 — minimal external kernel application module
Phase 2 — kernel bus discovery
Phase 3 — generic pre-activation SDO mechanism
```

and before:

```text
Phase 4 — finalising generic PDO/domain configuration
Phase 5 — cyclic kernel loop
IOD integration
```

Phase 3 should therefore provide enough mechanism to reproduce the existing `sdo.sh`, but the raw setup-SDO interface must remain provisional until this decision gate is completed.

The sequence is:

```text
Phase 1
    prove external kernel module and master ownership
        |
Phase 2
    prove slave discovery
        |
Phase 3
    prove explicit ordered SDO setup
        |
        v
+----------------------------------------------+
| PDO SETUP MECHANISM DECISION GATE            |
|                                              |
| Test A: existing explicit SDO sequence        |
| Test B: EtherLab declarative PDO setup        |
| Compare actual servo object dictionary        |
| Test power-cycle/reconfiguration behaviour    |
+----------------------------------------------+
        |
        +-- declarative setup proven reliable
        |       |
        |       v
        |   use ecrt_slave_config_pdos()
        |   for ED3L PDO mapping
        |
        +-- explicit SDO setup required
                |
                v
            retain generic ordered
            pre-configuration SDO mechanism
        |
        v
Phase 4
    finalise kernel configuration ABI
```

This is deliberately early in development so that the result can influence the kernel ABI before IOD depends on it.

---

## Test A — establish the known-good ED3L baseline

Use the existing `sdo.sh` sequence or the Phase 3 generic SDO test tool to configure one ED3L servo.

Record every write and verify the final values by uploading the relevant objects.

At minimum read back:

```text
0x1600:00
0x1600:01
0x1600:02
0x1600:03
0x1600:04
0x1600:05
0x1600:06

0x1A00:00
0x1A00:01
0x1A00:02
0x1A00:03
0x1A00:04

0x1C12:00
0x1C12:01

0x1C13:00
0x1C13:01

0x6060:00
```

Expected mapping:

```text
RxPDO 0x1600

0x6040:00 / 16
0x60FF:00 / 32
0x6083:00 / 32
0x6084:00 / 32
0x60E0:00 / 16
0x60E1:00 / 16

TxPDO 0x1A00

0x6041:00 / 16
0x606C:00 / 32
0x6077:00 / 16
0x603F:00 / 16
```

Save the baseline output in a test artifact rather than relying on visual comparison.

For example:

```text
tests/ethercat/expected/ed3l_velocity_pdo.txt
```

or a structured equivalent.

---

## Test B — configure the same PDO layout using EtherLab's declarative PDO API

Return the servo to a known initial state.

This may require:

```text
drive power cycle
slave reset
restoring factory/default PDO mapping
```

Determine and document the correct repeatable reset procedure.

Do not compare Test B against a servo that still contains the mapping written by Test A.

Create a minimal standalone EtherLab kernel configuration using the same desired PDO entries.

Conceptually:

```text
slave_config = ecrt_master_slave_config(...)

ecrt_slave_config_pdos(
    slave_config,
    ... desired sync/PDO/entry structure ...
)

ecrt_slave_config_sdo8(
    slave_config,
    0x6060,
    0x00,
    3
)
```

Activate/configure the slave through the normal EtherLab lifecycle.

Then read back exactly the same objects used in Test A.

Compare:

```text
Test A explicit SDO result
        versus
Test B declarative EtherLab result
```

The comparison must verify the actual slave object dictionary, not merely that EtherLab accepted the configuration call.

---

## Test C — operational PDO test

Matching object-dictionary values alone are insufficient.

For both Test A and Test B, verify that the servo:

```text
reaches the expected EtherCAT state
reaches OP reliably
reports a correct working counter
accepts Control Word
accepts Target Velocity
reports Status Word
reports Actual Velocity
reports Actual Torque
reports Error Code
```

Where safe, test actual servo enable and low-speed movement according to the machine's normal commissioning/safety procedure.

Do not perform unexpected motion merely to automate this test.

---

## Test D — power-cycle and automatic reconfiguration behaviour

This test is particularly important.

The existing `sdo.sh` approach is run once before IOD starts.

Determine what happens when:

```text
IOD/kernel application remains running
        |
ED3L loses control power
        |
ED3L powers back up
```

For the declarative EtherLab configuration path, determine whether EtherLab automatically restores:

```text
0x1600
0x1A00
0x1C12
0x1C13
0x6060
```

as the slave progresses back toward PREOP/SAFEOP/OP.

For the explicit SDO recipe path, determine whether the kernel application must detect slave reconfiguration and rerun the recipe.

This may materially affect which mechanism is preferable.

Record the observed state transitions and object values.

---

## Test E — repeated configuration lifecycle

For each candidate mechanism, repeat:

```text
configure
activate
run
deactivate
reset
configure
activate
run
```

many times.

Verify:

```text
no SDO aborts
no failed transitions to OP
no stale PDO mapping
no EtherLab errors
no kernel memory leak
no dangling configuration objects
```

The declarative mechanism should not be selected merely because it worked once.

---

## Decision outcomes

### Outcome 1 — EtherLab declarative PDO setup is fully reliable

If:

```text
ecrt_slave_config_pdos()
```

produces the required physical ED3L mapping, reaches OP reliably, survives repeated activation cycles, and is correctly reapplied after servo power loss/reconfiguration, then prefer:

```text
Clockwork desired PDO description
        |
kernel builds EtherLab PDO configuration
        |
ecrt_slave_config_pdos()
        |
EtherLab owns PDO reconfiguration lifecycle
```

Use normal startup/configuration SDO support for values such as:

```text
0x6060:00 = 3
```

The generic raw ordered setup-SDO mechanism may still be useful for other devices, but ED3L no longer depends on it.

### Outcome 2 — ED3L requires explicit ordered SDO PDO setup

If the declarative path fails or is unreliable, retain:

```text
Clockwork selects ED3L setup recipe
        |
kernel executes ordered SDO sequence
        |
kernel configures domain to match resulting PDO layout
        |
activate
```

The kernel remains device-agnostic.

The ED3L-specific sequence remains data/policy owned by user space.

The kernel API must support a generic ordered pre-configuration SDO transaction mechanism.

### Outcome 3 — hybrid mechanism required

It is possible that the drive requires explicit SDO writes for some mapping operations but EtherLab should still be given the declarative PDO description for domain/FMMU configuration.

If so, explicitly document:

```text
which objects must be written manually
which objects EtherLab configures
the required order
which component owns reapplication after slave restart
```

Do not allow both mechanisms to unknowingly write the same mapping objects during one configuration transition.

---

## Critical implementation warning

If the explicit SDO mechanism is retained, determine exactly **when** those writes can safely occur relative to EtherLab's own slave configuration state machine.

The kernel module must not create a race where:

```text
custom SDO recipe writes 0x1600/0x1A00
```

while:

```text
EtherLab simultaneously rewrites 0x1600/0x1A00
```

through its normal PDO configuration.

The final design must have one clearly defined owner for each PDO mapping operation.

---

## Decision-gate deliverable

Before Phase 4 is considered complete, create:

```text
docs/ethercat-kernel/ed3l-pdo-configuration-test.md
```

It must contain:

```text
target EtherLab version
ED3L vendor/product/revision
servo firmware version if available
initial PDO mapping
explicit SDO test result
declarative EtherLab test result
object dictionary readback comparison
OP transition result
working-counter result
power-cycle/reconfiguration result
repeated lifecycle result
selected production mechanism
reason for selection
```

The implementation plan must not assume the answer before this document exists.

# 11. Phase 4 — configure PDOs and create an EtherLab domain from user-supplied data

## Goal

Allow user space to supply the same generic configuration currently held by `ECModule`.

Still do not connect IOD initially.

## Configuration model

The UAPI should represent:

```text
slave
    |
    +-- sync manager
            |
            +-- PDO
                    |
                    +-- PDO entry
```

Possible structures:

```c
struct cw_ec_slave_cfg {
    __u32 config_id;
    __u16 alias;
    __u16 position;
    __u32 vendor_id;
    __u32 product_code;
    __u32 revision_number; /* matching/diagnostic only if required */
};

struct cw_ec_sync_cfg {
    __u32 slave_config_id;
    __u8 sync_index;
    __u8 direction;
    __u8 watchdog_mode;
    __u8 reserved;
};

struct cw_ec_pdo_cfg {
    __u32 sync_config_id;
    __u16 pdo_index;
    __u16 reserved;
};

struct cw_ec_pdo_entry_cfg {
    __u32 pdo_config_id;
    __u32 entry_id;
    __u16 index;
    __u8 subindex;
    __u8 bit_length;
};
```

These are conceptual only.

Codex must design the real API after inspecting how Clockwork's current `DeviceInfo`, `ECModule`, `entry_details`, offsets and bit positions are used.

## Stable entry IDs

Strongly consider assigning every requested process item a user-space-provided stable `entry_id`.

Example:

```text
entry_id = 1001
slave = 5
index = 0x6041
subindex = 0
bits = 16
```

The kernel can then return:

```text
entry_id = 1001
global_offset = 42
bit_position = 0
```

This avoids relying on array order as an implicit API.

## First standalone PDO tool

Create:

```text
tools/cw_ec_config
```

It should be able to read a simple test configuration independent of Clockwork.

For example:

```text
tools/test-configs/simple_el1008.json
tools/test-configs/simple_el2008.json
tools/test-configs/ed3l_velocity.json
```

The tool should:

1. open the device;
2. wait for bus-ready;
3. submit slave config;
4. submit Sync Manager config;
5. submit PDO config;
6. submit entry config;
7. validate;
8. ask the kernel to create EtherLab slave configs/domain registrations;
9. print returned offsets;
10. optionally activate.

## Validation requirements

Before calling EtherLab configuration functions, reject:

- duplicate IDs;
- invalid slave positions;
- identity mismatch where strict matching is requested;
- invalid Sync Manager direction;
- impossible bit lengths;
- PDO referencing a nonexistent Sync Manager;
- entry referencing nonexistent PDO;
- configuration after activation unless a defined deactivate/reset path exists;
- excessive counts or allocation sizes.

## Acceptance criterion

A standalone utility can configure a simple known Beckhoff terminal and activate the EtherLab master without Clockwork.

---

# 12. Phase 5 — cyclic kernel loop

## Goal

Run the deterministic EtherCAT cycle in kernel context.

## Required cycle

Conceptually:

```c
for (;;) {
    wait_for_next_period();

    ecrt_master_receive(master);
    ecrt_domain_process(domain);

    publish_input_process_data();
    consume_output_process_data();

    /* DC operations if enabled and appropriate */

    ecrt_domain_queue(domain);
    ecrt_master_send(master);
}
```

The exact sequence must match the existing implementation and EtherLab recommendations.

## Timing mechanism

Do not choose the mechanism casually.

Investigate:

- high-resolution kernel timers;
- dedicated `kthread`;
- `hrtimer` wake-up of RT kthread;
- PREEMPT_RT behaviour;
- CPU affinity;
- scheduling priority;
- interaction with EtherLab network driver's polling/IRQ behaviour.

The goal is not merely "kernel = real time."

Measure it.

## Cycle frequency

Current code indicates a default EtherCAT frequency of 2000 Hz in `ECInterface`.

The kernel API should accept cycle time/frequency during configuration.

Do not hard-code 2 kHz.

Possible configuration:

```text
cycle_period_ns = 500000
```

Prefer period in nanoseconds over frequency if that simplifies timing and avoids division ambiguity.

The selected period is user-space policy supplied at activation and is
immutable until deactivation. If DC is configured, submitted SYNC0 periods
must agree with it. Keep the transport timer distinct from EtherLab's own
operation-FSM send interval: the target 1.6.9 header declares
`ecrt_master_set_send_interval()`, but the installed kernel module does not
implement/export that symbol. Do not add an unmaintainable EtherLab fork or
call an internal unexported symbol. Detect and document the installed
capability, and do not claim the FSM interval changed when its supported
kernel API is absent.

## CPU affinity and priority

The existing system already has:

- CPU governor configuration;
- machine-specific IRQ affinity;
- user-space SCHED_FIFO and CPU-affinity handling.

The new kernel thread needs deliberate placement.

Expose module parameters or configuration values for:

```text
cycle_cpu
cycle_priority
```

But first investigate the correct kernel scheduling APIs and the target PREEMPT_RT kernel.

Do not automatically copy user-space priority numbers without confirming their meaning.

## Statistics

Add low-overhead counters:

```text
cycles
late_cycles
maximum_lateness_ns
last_lateness_ns
minimum_period_ns
maximum_period_ns

rx_count / tx_count if available
working_counter state
domain WC state
link state
slave operational count
```

Counters must not require per-cycle formatted logging.

## Standalone cycle test

Create:

```text
tools/cw_ec_cycle
```

It should:

1. submit a known config;
2. activate;
3. run for N seconds;
4. show cycle statistics;
5. show master/domain state;
6. deactivate cleanly.

## Acceptance criterion

Run for an extended period and compare:

- current IOD user-space cycle timing;
- kernel cycle timing.

Do not claim improvement without measurements.

---

# 13. Phase 6 — process image sharing with user space

## Goal

Allow a normal user-space process to consume inputs and update outputs without participating in EtherCAT packet timing.

## Candidate architecture

Use a memory-mapped region associated with:

```text
/dev/cw_ethercat0
```

Conceptually:

```text
+--------------------------------------------------+
| shared header                                    |
|   ABI version                                    |
|   state                                          |
|   generation                                     |
|   input sequence                                 |
|   output sequence                                |
|   cycle count                                    |
|   status flags                                   |
+--------------------------------------------------+
| user-visible input image                         |
+--------------------------------------------------+
| user-visible output image                        |
+--------------------------------------------------+
```

## Important decision: direct domain mmap vs shadow images

Do **not** expose EtherLab's raw domain memory directly until concurrency has been carefully analysed.

A safer first design is:

```text
EtherLab domain process data
        |
kernel copies input section
        |
shared input shadow

shared output shadow
        |
kernel copies output section
        |
EtherLab domain process data
```

At 2 kHz and modest process-image sizes, a copy may be negligible and greatly simplify correctness.

Optimise to zero-copy only after measurement.

## Concurrency scheme

Define exactly how user space knows a coherent input snapshot is available.

Possible mechanisms:

### Sequence counter

Kernel:

```text
seq++
copy inputs
seq++
```

User space retries if the sequence changed or was odd.

### Double buffer

Kernel writes inactive buffer then flips generation/index.

### Explicit cycle counter with barriers

Only if correctness can be demonstrated.

Similarly define how output updates become visible to the kernel.

Do not depend on ordinary compiler ordering.

Use appropriate kernel/user-space memory barriers or an ABI pattern that avoids subtle races.

## Inputs vs outputs

Determine from current domain registrations which entries are inputs and outputs.

The kernel needs enough metadata to know:

- which bytes/bits it publishes to user space;
- which bytes/bits it consumes from user space.

Alternatively expose one shadow image mirroring the domain and preserve masks.

Investigate the current `IOComponent`/process-data layout before deciding.

## Acceptance criterion

A standalone utility can:

- toggle a configured Beckhoff digital output;
- observe a configured digital input;
- do this while the kernel owns the 2 kHz EtherCAT cycle.

No Clockwork involved.

---


# 13A. Future motion-control compatibility requirements

## Objective

The primary purpose of this project is to provide a deterministic, generic EtherCAT transport layer.

The initial implementation **is not** a motion controller.

However, the architecture and UAPI **must** support future deterministic motion-control applications, including CiA 402 Cyclic Synchronous Position (CSP), without requiring redesign of the transport layer.

The first implementation should therefore expose the timing and synchronisation information required by future motion-control software while deliberately avoiding motion-specific policy.

---

## Design principle

The kernel module owns **EtherCAT timing**.

Clockwork owns **application behaviour**.

Future motion-control components should consume the kernel's timing information rather than attempting to become the EtherCAT timing source.

Conceptually:

```text
Clockwork
        |
machine logic
        |
        v
generic EtherCAT transport
        |
deterministic EtherCAT cycle
```

Future motion control should become:

```text
Clockwork
        |
high-level commands
        |
        v
Motion planner
        |
cycle-addressed setpoints
        |
        v
generic EtherCAT transport
        |
EtherCAT network
        |
CiA 402 servo
```

The transport layer should remain reusable for many different EtherCAT devices.

---

## Kernel timing responsibilities

The kernel module shall become the authoritative owner of the EtherCAT cycle.

Every completed EtherCAT cycle should produce timing information including, where applicable:

```text
cycle number
configured cycle period
scheduled cycle time
actual wake time
cycle lateness
input generation
output generation consumed
working counter
working counter state
missed deadline count
stale output count
driver status flags
distributed clock timing information
```

The first useful cyclic prototype should implement the core timing fields rather than leaving all timing publication until later.

At minimum, the initial cyclic API should expose:

```text
cycle number
configured cycle period
scheduled cycle time
actual cycle time
input generation
output generation consumed
cycle lateness
working counter
missed deadline count
stale output count
```

Additional timing and Distributed Clock fields may be added in later compatible ABI revisions.

---

## Clockwork timing model

Clockwork should not attempt to become the EtherCAT timing source.

Instead, Clockwork should:

```text
read coherent process data
determine which EtherCAT cycle produced that data
run machine logic
publish desired outputs
```

Clockwork may execute at the EtherCAT cycle rate if desired, but the correctness of EtherCAT packet transmission must not depend on user-space wake-up latency.

The kernel continues exchanging EtherCAT frames regardless of temporary user-space scheduling delays.

Clockwork should be able to wait for a new input generation when cycle-synchronised application behaviour is required, but failure or delay in that wait must not delay the kernel's EtherCAT transmission cycle.

The initial API should therefore allow a future notification mechanism such as:

```text
poll/epoll notification
blocking wait-for-cycle operation
event notification associated with a new input generation
```

The first implementation may choose the simplest safe mechanism. It must not require busy-waiting as the only supported user-space synchronisation method.

---

## Initial timing API

The initial implementation should expose sufficient timing information for diagnostics, Clockwork scheduling and future deterministic applications.

Conceptually:

```c
struct cw_ec_cycle_info {
    __u32 struct_size;
    __u32 flags;

    __u64 cycle_index;
    __u64 cycle_period_ns;

    __u64 scheduled_time_ns;
    __u64 actual_time_ns;

    __u64 input_generation;
    __u64 output_generation_consumed;

    __s64 wake_lateness_ns;

    __u32 working_counter;
    __u32 working_counter_state;

    __u64 missed_deadlines;
    __u64 stale_output_cycles;
};
```

The exact structure may differ after UAPI design and compatibility review, but the concepts should remain.

Use Linux fixed-width UAPI types rather than native C types in exported kernel headers.

The timing structure represents the EtherCAT transport timeline rather than application or wall-clock timing.

The meaning of each timestamp must be documented precisely, including:

```text
which clock source is used
where in the cyclic sequence the timestamp is captured
whether the value is scheduled or observed
whether values remain monotonic across deactivate/activate
when cycle_index resets
```

Do not publish ambiguously named timing fields.

---

## Process-image generation semantics

The initial process-image API shall define generation semantics clearly.

At minimum:

```text
input_generation
```

identifies the completed EtherCAT cycle whose coherent input image is visible to user space.

```text
output_generation
```

identifies a complete output image published by user space.

```text
output_generation_consumed
```

identifies the most recent user-space output generation copied into or otherwise consumed by the kernel cyclic path.

The API documentation must define whether an output generation is considered consumed when it is:

```text
accepted from user space
copied into the EtherLab domain
queued for transmission
sent by ecrt_master_send()
```

Choose one meaning and use it consistently.

The preferred initial interpretation is that `output_generation_consumed` identifies the generation selected by the kernel for the current EtherCAT cycle. Separate transmitted/acknowledged concepts can be added later if technically meaningful.

Clockwork must be able to determine:

```text
which cycle produced the inputs it is processing
which user-space output generation the kernel used
whether the same output image has been reused for multiple cycles
```

---

## Time base and cycle identity

The authoritative deterministic time reference for the generic API should be:

```text
cycle_index
configured cycle_period_ns
```

Wall-clock time must not be required for deterministic application behaviour.

Timestamps should use an explicitly documented monotonic kernel clock suitable for interval measurement.

If Distributed Clocks are enabled later, the API may expose the relationship between:

```text
kernel application time
EtherCAT reference clock time
Distributed Clock phase/error information
```

Clockwork should receive this as timing and diagnostic data.

Clockwork should not independently perform EtherLab Distributed Clock correction after the kernel owns the cyclic loop.

---

## API extensibility requirements

The initial UAPI shall be versioned.

The initial ABI shall not prevent future addition of features such as:

```text
cycle-addressed output updates
scheduled output queues
buffered output queues
distributed clock diagnostics
cycle-triggered notifications
runtime SDO improvements
future motion extensions
```

The initial implementation does **not** need to implement these capabilities.

It must avoid making them impossible or forcing incompatible changes to the existing process-image and configuration interfaces.

In particular:

1. Do not assume that "latest output wins" is the only output-delivery model the module will ever support.

2. Do not embed servo-specific fields into the generic process-image header.

3. Keep process-image transport and any future scheduled-output mechanism as separate concepts.

4. Use feature flags or capability queries so user space can discover later optional facilities.

5. Do not require increasing the major API version merely to add a separate optional scheduled-output interface.

6. Do not expose kernel pointers or internal EtherLab objects as future scheduling handles.

---

## Capability discovery

Include or reserve a capability-discovery mechanism.

Conceptually:

```text
GET_API_VERSION
GET_CAPABILITIES
```

Possible future capability flags include:

```text
coherent process image
cycle timing information
cycle notification
distributed clock diagnostics
scheduled output queue
runtime SDO requests
```

Do not report a capability until its semantics are implemented and documented.

This allows future Clockwork or motion software to use advanced features without assuming that every deployed module version supports them.

---

## Explicitly deferred functionality

The following are intentionally **outside the scope** of the first implementation:

```text
CiA 402 Cyclic Synchronous Position (CSP)

trajectory generation

motion interpolation

jerk-limited motion

coordinated multi-axis motion

CNC functionality

servo planner

following-error supervision

motion queue management

servo-specific underrun policy

CiA 402 state-machine policy
```

These require significantly more design work and should not complicate the initial EtherCAT transport implementation.

No first-version kernel API should present these as partially supported production features.

---

## Expected future CSP architecture

If CSP support is later added, the preferred architecture is expected to resemble:

```text
Clockwork
        |
high-level motion request
        |
        v
Motion planner
        |
cycle-addressed target positions
        |
        v
etherlab-cyclic-kmod
        |
deterministic EtherCAT cycle
        |
CiA 402 CSP servo
```

The kernel should remain responsible for deterministic EtherCAT transport and exact cycle selection.

The motion planner should remain replaceable.

The transport layer should not become permanently coupled to one motion-control implementation.

A future CSP experiment may initially use a dedicated standalone real-time user-space tool or motion service rather than IOD.

This allows CSP timing, queueing and underrun behaviour to be evaluated independently before choosing a permanent Clockwork integration design.

---

## Future scheduled-output concept

The initial implementation shall not freeze a scheduled-output ABI.

It should nevertheless preserve a clean architectural path for a later queue containing output data associated with a future EtherCAT cycle.

Conceptually only:

```c
struct cw_ec_scheduled_output {
    __u32 struct_size;
    __u32 flags;

    __u64 configuration_generation;
    __u64 target_cycle;

    __u32 output_group_id;
    __u32 payload_length;

    /* payload supplied through a future bounded interface */
};
```

This is not a required first-version structure and must not be copied directly into the production UAPI without further design.

A later design must determine:

```text
queue ownership
queue depth
memory limits
how points are submitted
how late points are rejected
how configuration generations are checked
how underrun is reported
what occurs on deactivate
whether queues are per-domain, per-output group or per-client
```

The initial API should simply avoid conflicting assumptions.

---

## Future CSP safety and underrun decisions

Future CSP support will require explicit decisions about what happens when the next scheduled target is unavailable.

Possible policies may include:

```text
hold last target
controlled stop
quick stop
disable operation
drop drive enable
```

The generic transport module must not silently choose a servo-motion safety policy during the first implementation.

Any future scheduled-output interface must:

```text
reject points for cycles that have already passed
report queue depth
report late-point rejection
report underruns
associate queued data with a configuration generation
define behaviour during deactivate and link loss
```

The appropriate machine response remains a later safety and motion-control design decision.

Software behaviour does not replace hardware safety systems.

---

## Distributed Clock compatibility

The initial timing design shall not make future Distributed Clock use difficult.

If the kernel owns the cyclic loop, it should eventually own the consistently placed EtherLab calls associated with:

```text
application time
reference clock synchronisation
slave clock synchronisation
reference-clock diagnostics
```

The initial UAPI should allow compatible addition of:

```text
reference clock availability
application/reference time difference
phase or offset diagnostics
maximum observed DC deviation
DC synchronisation status flags
```

Do not require any user-space controller to duplicate or compete with
kernel-owned Distributed Clock timing.

### User-space control-clock contract

The current API 0.13 cycle record is adequate for monitoring but not sufficient
for a tightly scheduled user-space control loop. A compatible later UAPI must
associate each completed global cycle with the exact DC/application-time state
used by the kernel for that cycle.

The coherent record shall include:

```text
configuration generation
global cycle index
scheduled monotonic wake time
actual monotonic wake time
exact 64-bit application_time_ns passed to EtherLab
DC reference-valid state
the low-32-bit reference-clock sample returned by EtherLab 1.6.9
normalized application/reference phase difference
cycle adjustment applied to application-time progression
input generation produced by the cycle
output generation selected for the cycle
```

User space follows this kernel-published cycle and application-time mapping.
It must not independently call EtherLab clock APIs, recreate the DC steering
algorithm, or assume that nominal period multiplication gives the exact future
application time while steering is active.

The intended motion-capable timeline is:

```text
kernel cycle N:
  wake
  receive/process
  sample DC reference
  publish input N
  select output already queued for N
  advance and publish EtherLab application time
  synchronize/queue/send
  publish the coherent cycle-N record and wake user space

user-space controller:
  wake from cycle-N record
  read input tagged N
  compute output for explicit cycle N + lead
  enqueue before that target cycle's admission deadline
```

A wake after cycle N is observational; it cannot change the datagram already
sent for N. Ordinary latest-shadow output may be published for a following
cycle, but motion control requires the separately designed bounded
cycle-addressed queue above. Queue lead, admission deadline, capacity,
underrun indication and fail-safe response must be explicit.

Because existing ioctl command encoding includes structure size, do not enlarge
the API 0.13 `cw_ec_cycle_info` in place. Add a new versioned timing-record and
wait operation, while continuing to serve the API 0.13 record for binary
compatibility.

---

## Documentation requirements

Document the timing and generation model in:

```text
docs/uapi.md
docs/architecture.md
```

At minimum, document:

```text
cycle_index lifetime and reset behaviour
cycle_period_ns
timestamp clock source
timestamp capture points
input_generation publication
output_generation publication
output_generation_consumed semantics
stale-output counting
missed-deadline counting
notification semantics
memory ordering required by user space
capability discovery
```

A developer integrating another user-space application should not need to inspect kernel code to understand the transport timeline.

---

## Initial implementation acceptance criteria

Before IOD integration begins, demonstrate that:

1. Every EtherCAT cycle has a unique, monotonically increasing cycle number while the cyclic engine is running.

2. The configured cycle period is available to user space.

3. User space can determine which EtherCAT cycle produced the current coherent input image.

4. User space can determine which output generation the kernel selected for transmission.

5. User space can detect when the same output generation has been reused for multiple cycles.

6. Scheduled and actual cycle timing can be compared.

7. Cycle lateness and missed deadlines are measurable without per-cycle kernel logging.

8. Working-counter state is associated with the published runtime status.

9. The UAPI versioning and capability model support future compatible extension.

10. The current ABI contains no assumptions that prevent future cycle-addressed output scheduling.

11. No servo-specific concepts have been embedded into the generic transport API.

12. The standalone tools can display and record the timing fields for benchmark comparison.

13. Timing and generation semantics are documented sufficiently for an independent user-space client.

---

## Deferred CSP decision gate

Do not finalise a production CSP or scheduled-output API as part of the initial transport implementation.

After the normal process-image path is stable, create a separate design and experiment phase covering:

```text
ED3L CSP support and PDO mapping
CiA 402 mode 8 behaviour
Distributed Clock requirements
cycle-addressed setpoint buffering
minimum safe queue depth
user-space real-time planner behaviour
queue underrun behaviour
drive following-error behaviour
multi-axis synchronisation
controlled stopping
Clockwork integration
```

Only after those tests should the project decide whether motion setpoint generation belongs in:

```text
Clockwork
a dedicated real-time user-space motion service
a separate motion kernel module
or another architecture
```

The generic kernel transport should not be redesigned merely because one experimental motion architecture was chosen.

---

# 13B. Controller liveness and loss-of-control policy

## Failure modes

Controller process exit and controller loss of control are different events.

If the exclusive controller exits or closes its file descriptor, the kernel
release path can synchronously gate outputs, stop cyclic work, release the
master and free the controller-owned configuration.

A controller may instead deadlock, stop scheduling, lose its upstream command
source, or otherwise retain the file descriptor without publishing control
updates. File lifetime alone cannot detect this condition. Reusing the last
armed output indefinitely is not an acceptable implicit policy.

## Required transport behaviour

Add a generic, bounded controller lease/heartbeat facility in a compatible
UAPI revision. User space owns the timeout choice and the application decision
to renew the lease. The kernel owns deterministic expiry detection and output
gating.

The design shall distinguish:

```text
controller file closed
controller lease expired while the file remains open
ordinary output-image reuse within a valid lease
future scheduled-output queue underrun
```

The initial controller-liveness design should provide:

```text
pre-activation lease configuration
explicit renewal by the exclusive control owner
cycle-based or monotonic expiry checked by the cyclic kernel task
low-overhead lease state and expiry statistics
a distinct controller-stale fault cause
```

On lease expiry while outputs are armed, the kernel shall:

1. atomically close the output gate;
2. select the zero/fail-safe process image on the next deterministic cycle;
3. continue EtherCAT receive/process/queue/send so inputs and recovery remain
   observable;
4. latch `rearm_required` and a controller-stale fault;
5. increment an expiry counter; and
6. require lease renewal, a fresh output generation and explicit re-arm before
   nonzero output can be selected again.

Renewing a lease must never arm outputs. Expiry must not silently stop the
EtherCAT transport, release the master, or substitute for hardware safety.

## Initial design decisions

The first compatible implementation shall use these decisions:

- Lease enforcement is explicit opt-in during migration. A disabled lease
  preserves API 0.13 arm behaviour; an enabled lease is mandatory for every
  arm.
- The authoritative timeout is an unsigned count of armed output cycles.
  Accepted limits are 1 through 1,000,000 cycles. No time conversion occurs in
  the kernel; user space chooses the count using its configured cycle period.
- The budget decreases once immediately before output selection in each armed
  cycle. A budget of `N` therefore permits exactly `N` cyclic selections of
  the retained output shadow.
- The budget pauses while outputs are disarmed. Monitoring and input exchange
  can consequently continue indefinitely without a heartbeat.
- Enabling the lease is pre-activation configuration. Each activation begins
  with an invalid lease and zero remaining cycles. The exclusive controller
  must explicitly renew before arm.
- Renewal restores the configured cycle budget but never publishes output,
  clears `rearm_required`, or arms outputs.
- Expiry is checked by the cyclic task before output selection. That cycle
  selects zeros, latches the controller-stale fault and records the current
  output sequence. Recovery therefore requires renewal, a newer output
  publication and explicit arm.
- User space can deliberately stop renewal when it loses upstream command
  authority. The kernel does not interpret the upstream cause.
- Orderly disarm, deactivation and close do not count as expiry. Deactivation
  and close invalidate the lease as part of normal teardown.
- Controller-lease expiry and a future scheduled-output queue underrun use
  different fault bits and counters.

This cycle-counted output-authority model makes expiry deterministic relative
to the EtherCAT cycle and keeps application policy out of the kernel.

## Required tests

Before enabling the lease in IOD, standalone tests must cover:

```text
expiry while armed with a retained file descriptor
continued cyclic input exchange after expiry
zero-gate acknowledgement
renewal immediately before the boundary
late renewal that cannot resurrect outputs
fresh publication plus explicit re-arm after renewal
controller close during expiry
deactivation during expiry
counter and arithmetic boundaries
hostile timeout, generation, flags and reserved fields
```

Initial hardware tests remain zero-output or use only a separately authorized,
physically safe bounded commissioning output.

---

# 13C. Delegated per-domain user-space control

## Goal

Support optional independent user-space control connections for explicitly
declared domains without creating multiple EtherCAT masters or cyclic loops.
For example, a dedicated motion controller may own the drive domain while a
supervisory controller owns ordinary machine I/O. A separate motion-planning
process may supply trajectories to that motion controller through user-space
IPC; trajectory semantics do not belong in this kernel transport.

The kernel shall retain:

```text
one EtherLab master owner
one immutable configured topology
one application-time/DC timeline
one receive/process/queue/send cycle
one globally monotonically increasing cycle identity
```

Domains are transport availability, validity and ownership boundaries within
that single cycle. They are not independent EtherCAT networks and do not each
call `ecrt_master_receive()` or `ecrt_master_send()`.

## Ownership model

The initial design shall distinguish:

- one coordinator connection, which exclusively owns discovery,
  configuration, activation, deactivation and master lifetime;
- optional delegated domain connections, each restricted to an explicit set
  of configured domain IDs;
- at most one output writer for a domain;
- read-only monitoring connections as a later compatible extension.

The preferred Linux interface is for the coordinator to request a new
kernel-created domain file descriptor after configuration. The coordinator
can pass that fd to the intended controller using `SCM_RIGHTS`. This avoids
reopen races, global bearer tokens and device-node permission ambiguity.
Closing a delegated fd revokes only its output authority; closing the
coordinator gates every output, closes delegated authority and tears down the
transport in the existing safe order.

The current exclusive control fd remains a compatibility owner of all domains
when no delegation is requested.

## Domain data path

A delegated connection shall be able to:

```text
wait for the common cycle notification
read a coherent copied input segment with its exact global cycle identity
publish only bytes/bits registered as outputs in its owned domain
observe its domain WC, validity, fault and generation state
renew, arm and disarm only its own output authority
```

The cyclic task still assembles and sends every domain once per global cycle.
It selects the latest permitted output generation independently for each
domain. No domain connection may address, mask or modify another domain's
process-image segment.

Independent domain publication does not imply an atomic transaction across
domains. If an application needs multi-domain outputs to take effect on one
cycle, that requires a separately designed cycle-addressed group-commit API;
the kernel must not infer such coupling.

## Liveness and fault containment

Controller leases from Section 13B shall be represented internally as output
authority scoped to a domain set, even if the first exposed API revision
supports only the compatibility all-domain owner. This avoids baking a global
lease into the implementation.

Expiry or close of a delegated connection shall:

1. gate only its owned domain outputs to zero/fail-safe values before their
   next selection;
2. latch per-authority and affected-domain stale-controller state;
3. leave unrelated domain output authorities unchanged;
4. continue the common EtherCAT cycle and all input publication; and
5. require renewal/reconnection, fresh domain output publication and explicit
   per-domain re-arm.

Bus-level, topology, master and coordinator faults may still gate all domains.
Software domain isolation does not replace physical safety or drive-local
watchdogs.

## Design gates before implementation

Before exposing delegated fds, document and test:

```text
fd and coordinator lifetime ownership
revocation and teardown lock order
domain-set representation and immutable authorization
per-domain copied-buffer ownership
per-domain output generations, masks, arm state and leases
coherent cycle notification fan-out
writer exclusivity and duplicate delegation rejection
coordinator close with live delegated fds
delegated process death and hung-controller lease expiry
cross-domain fault containment
generation handling across deactivate/reactivate
resource limits for delegated connections
```

No Clockwork, CiA 402, servo, axis or motion-planner policy may enter this API.

---

# 14. Phase 7 — build a compatibility user-space library

## Goal

Avoid scattering raw ioctl/mmap code through IOD.

Create a user-space library that wraps the kernel ABI.

Possible names:

```text
libcwethercat
libclockwork_ethercat
```

Suggested interface:

```cpp
class CwEthercatTransport {
public:
    bool open(unsigned master = 0);
    MasterInfo masterInfo();
    std::vector<SlaveInfo> slaves();

    void beginConfiguration();
    void addSetupSdo(...);
    void addSlave(...);
    void addSync(...);
    void addPdo(...);
    void addPdoEntry(...);
    ConfigurationResult commitConfiguration();

    void activate();
    void deactivate();

    ProcessImageView processImage();

    Status status();
};
```

Keep this library independent of Clockwork's `MachineInstance`.

It should be reusable by:

```text
cw_ec_bus
cw_ec_sdo
cw_ec_config
cw_ec_cycle
iod
```

This is important.

The test tools and IOD should exercise the same ABI wrapper.

---

# 15. Phase 8 — IOD integration, discovery only

## Goal

Make IOD able to use kernel-reported bus topology while leaving the existing cyclic path intact if practical.

This is a transitional phase.

## Existing behaviour to preserve

The code around `setupEtherCatThread()` currently:

- calls `collectEtherCatModules()`;
- obtains a slave list;
- matches XML-configured modules by position/product/revision;
- builds replacement `ECModule` instances.

Refactor slave-list acquisition behind an interface.

Example:

```cpp
class EthercatBusDiscovery {
public:
    virtual std::vector<SlaveInfo> listSlaves() = 0;
};
```

Implement:

```text
LibEthercatBusDiscovery
KernelEthercatBusDiscovery
```

or another design consistent with the codebase.

Avoid a large rewrite solely for abstraction purity.

## Acceptance criterion

IOD can print/consume the same slave identity information via the new kernel transport.

XML matching behaviour remains unchanged.

---

# 16. Phase 9 — IOD sends XML-derived PDO configuration to kernel

## Goal

Keep Clockwork's current XML parser and decision-making, but replace direct user-space calls to:

```text
ecrt_master_slave_config
ecrt_slave_config_pdos
PDO registration functions
```

with calls through `libcwethercat`.

## Existing configuration data

The current code already reduces XML information into `ECModule` fields such as:

```text
syncs
sync_count
entry_details
num_entries
```

Codex should create a conversion layer:

```text
ECModule
    |
    v
Kernel configuration DTO
    |
    v
libcwethercat
```

Do not make `ECModule` depend directly on Linux ioctl structs.

Keep UAPI types separated from application model types.

## Offset handling

Today `ECModule` stores arrays such as:

```text
offsets
bit_positions
```

When the kernel performs domain registration, IOD must receive equivalent mapping data.

Possible transitional approach:

1. Clockwork assigns an entry ID for every `entry_detail`.
2. Kernel registers the entry.
3. Kernel returns offset/bit position associated with entry ID.
4. IOD populates the existing `ECModule::offsets` and `bit_positions`.

This lets the rest of the IOD IO mapping code remain largely unchanged initially.

This is preferable to redesigning all `IOComponent` access in the same commit.

## Acceptance criterion

For XML-configured Beckhoff modules:

- the same XML file is used;
- the same matching record is selected;
- the resulting sync/PDO/entry configuration is equivalent;
- the returned offsets allow existing Clockwork IO mapping code to function.

---

# 17. Phase 10 — migrate ED3L `sdo.sh` setup into Clockwork-driven kernel setup

## Goal

Remove the need for Bash to rewrite servo PDO mappings before starting IOD.

## Temporary transition

Keep `sdo.sh` available.

Add a configuration option:

```text
legacy_external_servo_pdo_setup = true/false
```

or a build/test switch.

Do not remove the known-good path until the new path is proven on a machine.

## New flow

```text
kernel scans bus
    |
IOD receives slave identities
    |
Clockwork matches ED3L devices
    |
Clockwork selects ED3L setup recipe
    |
Clockwork submits ordered setup SDO writes
    |
kernel executes writes
    |
kernel applies matching PDO/domain configuration
    |
kernel activates
```

## Where should the recipe live?

Do not decide prematurely.

Options:

1. Clockwork machine/module definition.
2. A separate device setup file.
3. Existing Clockwork library/device definition mechanism.
4. A structured representation generated from existing configuration.

The critical architecture is only:

```text
Clockwork owns recipe selection.
Kernel executes generic ordered typed writes.
```

## Validation

For every ED3L drive:

1. Apply old `sdo.sh`.
2. Upload and record:
   - `0x1600`;
   - `0x1A00`;
   - `0x1C12`;
   - `0x1C13`;
   - `0x6060`.

Then:

1. reset/reboot drive as necessary;
2. apply new kernel-driven recipe;
3. upload same objects;
4. compare byte-for-byte/logically.

## Acceptance criterion

`iod.sh` no longer needs to grep `ethercat slaves` or invoke `sdo.sh` for normal operation.

Keep the scripts for diagnostics/recovery initially.

---

# 18. Phase 11 — move the cyclic IOD path to mmap/shared process image

## Goal

Remove user-space responsibility for EtherCAT cycle timing.

Current IOD code should stop directly calling the cyclic EtherLab functions when the kernel backend is selected.

## Preserve the Clockwork processing model

IOD should continue to:

```text
read inputs
update Clockwork IO values
run machine logic
calculate outputs
publish outputs
```

But it should no longer determine when EtherCAT Ethernet frames are exchanged.

New conceptual model:

```text
Kernel:
    2 kHz
    receive EtherCAT
    process domain
    publish latest inputs
    consume latest outputs
    queue/send EtherCAT

IOD:
    existing application scheduling
    read latest coherent inputs
    run logic
    write desired outputs
```

## Important semantic question

Determine whether IOD currently relies on a strict one-to-one relationship between:

```text
EtherCAT receive cycle
Clockwork IO update
Clockwork logic execution
EtherCAT output transmission
```

If so, decoupling the rates can change behaviour.

Codex must analyse this before changing scheduling.

Potential design:

- kernel provides a cycle sequence;
- IOD can wait for a new EtherCAT input generation where desired;
- IOD may still run once per EtherCAT cycle, but user-space wake-up jitter no longer delays EtherCAT frame transmission;
- kernel can resend the last output image if IOD misses a cycle.

Explicitly define stale-output behaviour.

## Watchdog policy

Decide what kernel does if user space stops updating outputs.

Options:

1. continue last output indefinitely;
2. zero outputs;
3. apply configured safe image;
4. stop EtherCAT;
5. trigger EtherCAT watchdog behaviour.

Do not hard-code this without understanding current machine safety architecture.

Initially preserve current behaviour as closely as possible.

## Acceptance criterion

Machine IO behaves the same under normal operation while the kernel controls EtherCAT timing.

---

# 19. Phase 12 — runtime SDOEntry support

## Goal

Restore full `iod_sdo` functionality after the kernel owns `ec_slave_config_t` and the EtherLab application context.

The existing Clockwork semantics should remain in user space.

Clockwork currently has concepts including:

```text
SDOEntry name
module association
index
subindex
size
bit offset
VALUE
default
poll_interval
read request
write request
initial read
apply default after read
error state
```

These semantics should generally remain in IOD.

The kernel should own only the EtherLab request mechanism.

## Suggested split

Clockwork:

```text
"create runtime SDO handle for slave N, 0xXXXX:YY, size Z"
"read handle"
"write handle value"
"get completion"
```

Kernel:

```text
ecrt_slave_config_create_sdo_request
ecrt_sdo_request_read
ecrt_sdo_request_write
ecrt_sdo_request_state
request data buffer
```

## API model

Consider asynchronous request IDs:

```text
CREATE_SDO_REQUEST -> handle
SUBMIT_SDO_READ(handle)
SUBMIT_SDO_WRITE(handle, data)
GET_SDO_STATUS(handle)
GET_SDO_DATA(handle)
DESTROY_SDO_REQUEST(handle)
```

or shared request queues.

Do not perform blocking SDO work in the cyclic real-time thread.

If EtherLab async SDO request progress requires calls tied to the cyclic path, integrate only the minimum nonblocking state-machine interaction there.

Keep policy and polling schedule in IOD.

## Acceptance criterion

Existing Clockwork SDOEntry tests/behaviour work through the kernel backend.

---

# 20. Phase 13 — distributed clocks

The branch contains distributed-clock related EtherLab calls behind `USE_DC`.

Do not ignore these.

Before moving the cycle fully to kernel space, determine:

1. Whether production machines build with `USE_DC`.
2. Which machines/devices depend on it.
3. The exact current application-time algorithm.
4. Whether the new kernel cycle should own all DC operations.
5. How cycle timing changes affect DC correction.

If DC is in production use, the relevant logic probably belongs beside the kernel cyclic loop.

But preserve the existing algorithm first.

Do not redesign the DC controller during the transport migration.

Create separate tests and statistics for:

```text
reference clock availability
application-reference difference
maximum slave deviation
cycle adjustment
sync monitor timeout
```

---

# 21. Phase 14 — failure and recovery behaviour

Explicitly design and test:

## Link loss

What happens if:

```text
EtherCAT link drops for 100 ms
```

Does the kernel:

- keep cycling;
- report state;
- stop domain processing;
- automatically recover?

## Slave loss

What happens if a slave is disconnected?

## Slave replacement

What happens when the bus topology changes after startup?

Initial acceptable policy may be:

```text
report topology/config mismatch
do not silently reconfigure
require explicit stop/reconfigure/start
```

## IOD crash

The kernel module may continue transmitting last outputs if IOD dies.

This is a major behavioural change from a user-space EtherCAT owner.

Define a heartbeat/watchdog between IOD and kernel.

Potential fields:

```text
userspace_heartbeat
last_output_generation
last_userspace_update_ns
```

Define a configurable timeout.

But coordinate this with machine safety.

Do not imply software output clearing is a substitute for hardware safety circuits.

## Kernel module unload

Reject unload while active unless clean shutdown succeeds.

## Reconfiguration

Define:

```text
DEACTIVATE
RESET_CONFIG
SCAN
CONFIGURE
ACTIVATE
```

Do not attempt arbitrary hot PDO mutation in RUNNING state.

---

# 22. Phase 15 — IOD startup script changes

Only after the kernel path works.

Potential future `iod.sh`:

```bash
#!/bin/bash

BASEDIR='/opt/latproc'
ulimit -c unlimited

echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Ensure EtherLab and cw_ethercat kernel modules are loaded.
# Wait for the new transport to report link/bus readiness.

${BASEDIR}/code/machine/scripts/iod_irq_affinity.sh || \
    logger -p user.warn -t iod-run -- \
    "management IRQ policy returned an error"

# No servo-specific ethercat CLI PDO setup here.

exec nice -n-1 ${IOD} ...
```

Do not remove diagnostic CLI tools.

It remains useful to have:

```text
ethercat master
ethercat slaves
ethercat upload
ethercat download
```

available when the new kernel application is not actively owning the master.

---

# 23. Required standalone tools

The project should deliberately provide tools that let the architecture be tested without starting Clockwork.

At minimum:

## `cw_ec_bus`

Purpose:

```text
show kernel module state
show link state
show scanned slaves
show identity information
```

## `cw_ec_sdo`

Purpose:

```text
perform/test generic setup SDO reads/writes
execute an ordered recipe
```

## `cw_ec_config`

Purpose:

```text
submit slave/sync/PDO/entry configuration
validate it
show resulting registration offsets
```

## `cw_ec_cycle`

Purpose:

```text
activate configured domain
run kernel cyclic loop
show timing and bus statistics
```

## `cw_ec_io`

Purpose:

```text
read mapped inputs
set mapped outputs
inspect raw/shared process image
```

## `cw_ec_status`

Purpose:

```text
show runtime state
cycle counters
late-cycle counters
domain WC
slave states
user-space heartbeat status
```

These can eventually be one CLI with subcommands, but separate simple programs may be easier during development.

The tools should use the same user-space library intended for IOD.

---

# 24. Build system plan

**The kernel module, its UAPI/user-space library, and its standalone test tools should live in a new standalone project/repository, separate from the Clockwork repository.** Clockwork integration is a later consumer of that project's user-space interface.

There are three separate build products:

```text
1. Linux kernel module
2. user-space transport library
3. standalone diagnostic/test tools
4. modified IOD
```

## Kernel module

Likely use Kbuild.

Example conceptual command:

```bash
make -C /lib/modules/$(uname -r)/build M=$PWD modules
```

Need to add EtherLab include and symbol/module dependency requirements correctly.

Investigate whether EtherLab supplies:

```text
Module.symvers
```

and whether the external module requires:

```text
KBUILD_EXTRA_SYMBOLS
```

Do not copy random internet recipes without matching the installed EtherLab build.

## User-space build

Integrate with existing CMake where appropriate.

Possible layout:

```text
ethercat-kernel/
    include/
        cw_ec_uapi.h
    module/
        ...
    lib/
        ...
    tools/
        ...
```

Be careful that the shared UAPI header is usable by:

- kernel C;
- user-space C;
- user-space C++.

Avoid C++ constructs in UAPI.

## Packaging/install

Eventually support:

```text
cw_ethercat.ko
libcwethercat.so or static library
cw_ec_* tools
udev rule if needed
module-load config
```

Do not require world-writable `/dev/cw_ethercat0`.

Define appropriate group/permissions.

---


# 24A. Standalone project and EtherLab build dependency modes

The kernel module should be developed as a **standalone project/repository**, separate from Clockwork/IOD.

Suggested conceptual layout:

```text
clockwork-ethercat-kmod/
├── README.md
├── Makefile
├── kernel/
│   ├── Kbuild
│   ├── cw_ethercat_main.c
│   ├── cw_ethercat_master.c
│   ├── cw_ethercat_config.c
│   ├── cw_ethercat_cycle.c
│   ├── cw_ethercat_sdo.c
│   └── cw_ethercat_internal.h
├── include/
│   └── cw_ethercat_uapi.h
├── lib/
├── tools/
├── tests/
└── docs/
```

Clockwork should later consume the user-space library/UAPI from this project rather than containing the kernel module source itself.

The standalone project must support two EtherLab installation/build models used by existing machines.

## Build mode A — EtherLab master installed and rebuilt through DKMS

This is the normal case for newer systems.

The build investigation must determine, on an actual target machine:

```text
EtherLab DKMS package/module name and version
location of matching EtherLab headers, especially ecrt.h
location of the DKMS source tree
location of the DKMS per-kernel build tree
location of EtherLab Module.symvers
location of any required generated headers
how these paths vary by kernel version
```

Do not guess these locations.

Inspect the installed DKMS environment, including relevant locations such as:

```text
/usr/src/
/var/lib/dkms/
/lib/modules/$(uname -r)/
```

as applicable to the actual package.

The new module must build against artifacts corresponding to the **same kernel and EtherLab build** as the installed EtherLab master modules.

If symbol versioning is required, never silently consume a `Module.symvers` from a different kernel or EtherLab build.

The first development task should record the discovered DKMS layout in:

```text
docs/building/etherlab-dkms-environment.md
```

The standalone project's build system may provide an auto-detection helper, but auto-detection must:

1. identify the running or explicitly selected kernel version;
2. identify the installed EtherLab DKMS version;
3. locate the corresponding per-kernel DKMS build artifacts;
4. fail clearly if an unambiguous matching build cannot be found.

Allow explicit overrides even when auto-detection exists.

Conceptually the build should resolve:

```text
KERNEL_BUILD
ETHERLAB_INCLUDE
ETHERLAB_SYMVERS
```

before invoking Kbuild.

## Build mode B — manually built EtherLab source

Older systems have the EtherLab source available and the master is manually built.

Support explicit source/build paths, for example conceptually:

```bash
make \
    KERNEL_BUILD=/lib/modules/$(uname -r)/build \
    ETHERLAB_SRC=/path/to/etherlab/source \
    ETHERLAB_BUILD=/path/to/etherlab/build
```

The exact variables should reflect the real EtherLab tree after investigation.

From those paths, resolve:

```text
ecrt.h and required headers
Module.symvers if required
generated build headers if required
```

Do not require older production machines to be converted to DKMS merely to use the new module.

## Common build contract

Both modes should reduce to a common internal build contract:

```text
selected kernel release
kernel build directory
EtherLab API include path(s)
EtherLab symbol-version information if required
other generated EtherLab build artifacts if required
```

The kernel-module source itself should not care whether those artifacts originated from DKMS or a manual EtherLab build.

## Consider DKMS for the new module

After the standalone module is proven, consider providing a DKMS package for `cw_ethercat.ko` on newer systems.

Conceptually:

```text
ethercat-master DKMS
    |
    +-- builds/installs EtherLab modules for kernel X
    |
    v
clockwork-ethercat-kmod DKMS
    |
    +-- builds cw_ethercat.ko against the matching
        EtherLab API/symbol artifacts for kernel X
```

The dependency/build ordering must be tested.

Do not assume DKMS automatically makes another DKMS package's `Module.symvers` available.

The implementation must determine how the existing EtherLab DKMS packaging exposes the artifacts needed by dependent external modules.

For older systems, retain a normal manual build/install path.

## Build compatibility test matrix

Before IOD integration, test at least:

```text
new-system DKMS EtherLab + current target kernel
older manually built EtherLab + its target kernel
```

For each environment verify:

```text
clean build
module load
EtherLab symbol resolution
master acquisition/release
module unload
rebuild after kernel update where applicable
```

Record exact build instructions for both environments.



# 24B. Repository documentation and licensing requirements

The standalone project repository is:

```text
https://github.com/latproc/etherlab-cyclic-kmod
```

The repository must include clear end-user and developer documentation from the beginning. Documentation is part of the implementation, not a later cleanup task.

## README.md is required

A top-level `README.md` must exist before the first useful prototype is considered complete and must be kept accurate as the implementation evolves.

At minimum, the README must document:

### Project purpose

State that the project is:

> A generic Linux kernel module for deterministic EtherCAT cyclic I/O using the EtherLab master, with runtime user-space configuration of slaves, PDOs and SDOs.

Also state that the project is standalone, not Clockwork-specific, and that Clockwork/IOD is only one intended user-space consumer.

### Project status

Clearly identify whether the project is experimental, under development, or production-ready. During early development, prominently warn that faulty kernel modules can crash the host.

### Architecture

Explain the boundary:

```text
user-space application
        |
        | UAPI / user-space library
        v
/dev/cw_ethercat0
        |
        v
etherlab-cyclic-kmod
        |
        v
EtherLab master
        |
        v
EtherCAT network
```

Document that user space owns device matching, PDO-layout policy, setup-SDO policy and application semantics, while the kernel module owns EtherLab master lifecycle, configuration execution, domain registration and deterministic cyclic exchange.

### Requirements

Document tested Linux kernels, PREEMPT_RT expectations, supported/tested EtherLab versions, compiler/toolchain requirements, kernel build headers, EtherLab headers and any `Module.symvers` requirements.

Do not claim compatibility with untested versions.

### Building

Provide exact instructions for both supported environments:

- newer systems using EtherLab DKMS;
- older systems using manually built EtherLab source.

Document auto-detection and explicit override mechanisms.

### Installation

Document module installation, `depmod`, `modprobe`, device-node creation, udev rules or permissions, module parameters and uninstall procedure.

### Safe loading and unloading

Document how to load, inspect status and unload safely. Explain what must be stopped before unload and do not present forced module removal as normal operation.

### Usage

Document the lifecycle:

```text
load module
open device
check API version
inspect bus scan
retrieve slave topology
begin configuration
submit setup SDOs if required
submit slave/sync/PDO/PDO-entry configuration
validate
commit
activate
access process image
monitor status
deactivate
close
```

Include command examples.

### Standalone tools

Document each tool as it is implemented:

```text
cw_ec_bus
cw_ec_sdo
cw_ec_config
cw_ec_cycle
cw_ec_io
cw_ec_status
```

Explain which operations are diagnostic-only and which require master ownership.

### User-space interfacing

Document the public interface clearly enough that another project can integrate without reading kernel source.

This must cover:

```text
device node
API versioning
ioctl/message interface
configuration lifecycle
state machine
UAPI structs
entry IDs
error codes
process-image access
mmap/read/write semantics
memory-ordering requirements
configuration generations
runtime SDO interface when implemented
```

Detailed material may live in `docs/uapi.md`; the README should link to it.

### EtherCAT configuration model

Explain slaves, Sync Managers, PDO assignments, PDO mappings, PDO entries, startup/configuration SDOs and runtime SDOs.

Clearly distinguish declarative EtherLab PDO configuration from ordered pre-configuration SDO sequences.

### Testing

Document how to run:

```text
unit tests
no-hardware tests
bus-discovery tests
SDO tests
PDO configuration tests
cyclic timing tests
IO tests
lifecycle stress tests
malformed ABI tests
memory-leak tests
```

Include kernel-safety testing using `kmemleak`, KASAN, KFENCE and lockdep where supported.

### Development and contribution

Document development builds, debug/fault-injection options, coding expectations and required tests for changes affecting ownership, locking, teardown, UAPI or the cyclic path.

### Safety note

State clearly that this software may control industrial machinery, is not a substitute for hardware safety systems, and must be tested with machine motion safely inhibited during development.

### License

The repository is intended to use GNU General Public License version 2.

The repository must include a top-level `LICENSE` containing the GPL v2 license text.

Kernel source files should include:

```c
// SPDX-License-Identifier: GPL-2.0-only
```

and the kernel module should declare:

```c
MODULE_LICENSE("GPL");
```

Copyright headers should identify the actual copyright holder(s) and year(s).

Do not copy EtherLab source into this repository unless there is a specific reviewed reason.

If EtherLab-derived code is copied or adapted:

1. preserve all required copyright notices;
2. preserve all applicable license notices;
3. identify the source and version;
4. comply with GPL terms;
5. document the derivation in the relevant source file.

Do not remove third-party copyright or license notices.

### License scope of user-space components

The initial implementation may use GPL-2.0-only for the entire repository for simplicity.

If the user-space library or UAPI headers later need a different license, that must be an explicit, documented licensing decision. Do not silently mix licenses.

## Additional documentation files

As the project develops, create and maintain:

```text
README.md
LICENSE
docs/uapi.md
docs/architecture.md
docs/building.md
docs/testing.md
docs/safety-and-failure-behaviour.md
docs/etherlab-dkms-environment.md
docs/ed3l-pdo-configuration-test.md
```

These files may start small and grow with the implementation.

Avoid unnecessary duplication between README and detailed documents.

## Documentation acceptance gate

Before IOD integration begins, the repository documentation must allow a developer unfamiliar with Clockwork to:

1. understand the project purpose;
2. build against a supported EtherLab installation;
3. install and load the module;
4. run `cw_ec_bus`;
5. run the standalone configuration tests;
6. understand the lifecycle and state machine;
7. understand how user-space software interfaces with the module;
8. run the documented safety/memory tests;
9. unload/uninstall the module cleanly;
10. identify and follow the project's GPL v2 licensing requirements.

If these steps require undocumented knowledge from the Clockwork repository, the standalone project's documentation is incomplete.

# 25. ABI versioning

This is important because IOD and the kernel module may be updated independently on machines.

Include:

```c
#define CW_EC_API_VERSION_MAJOR ...
#define CW_EC_API_VERSION_MINOR ...
```

Have:

```text
GET_API_VERSION
```

IOD should fail clearly on incompatible major versions.

Configuration blobs/messages should contain:

```text
struct_size
version
```

where practical.

Do not expose kernel memory pointers.

---

# 26. Logging and diagnostics

Kernel logs should report:

```text
module load/unload
master request/release
link transitions
scan complete
configuration start/result
SDO setup failure
activation/deactivation
unexpected EtherLab error
cyclic thread start/stop
major bus-state transitions
```

Do not log every cycle.

Expose detailed counters through the device API, debugfs, or another deliberately chosen diagnostic interface.

Prefer the character-device API for information needed programmatically by IOD.

Use debugfs only for human diagnostics if desired.

---

# 27. Testing strategy

## Level 1 — build tests

- kernel module builds against target kernel;
- user library builds;
- tools build;
- IOD builds both old and new backends.

## Level 2 — no hardware

Where possible:

- ABI unit tests;
- config validation tests;
- serialization tests;
- entry-ID mapping tests;
- sequence-counter/shared-memory tests;
- fake backend tests.

Do not require EtherCAT hardware for every developer test.

## Level 3 — simple Beckhoff bus

Use a minimal known setup.

Test:

- discovery;
- one input terminal;
- one output terminal;
- XML-configured module if possible;
- activate/deactivate repeatedly.

## Level 4 — ED3L servo, motor disabled where possible

Test:

- SDO recipe;
- PDO mapping readback;
- process data;
- mode setting.

## Level 5 — machine test

Initially use a machine where outputs can be safely inhibited.

Compare:

```text
old user-space backend
new kernel backend
```

for:

- slave discovery;
- PDO offsets;
- IO values;
- cycle rate;
- working counter;
- link-loss behaviour;
- IOD restart behaviour.

## Level 6 — extended runtime

Run for many hours/days.

Record:

```text
cycle lateness
max jitter
WC changes
link changes
slave state changes
IOD heartbeat misses
kernel warnings
```

---

# 28. Benchmarking

The project exists partly to improve deterministic cyclic behaviour, so measure before and after.

Create a repeatable benchmark.

## Existing user-space backend

Collect:

```text
requested cycle period
actual cycle interval distribution
maximum late cycle
99th / 99.9th percentile lateness
EtherLab WC errors
CPU usage
context switches if useful
```

## Kernel backend

Collect same values.

Also test under:

```text
CPU load
disk IO
network load
MQTT traffic
Clockwork logging
HMI activity
```

Do not use only an idle-system benchmark.

### Live cycle-period transition

The generic controller may activate at a conservative period, establish the
required OP/domain-valid state, and then request its operating period without
rebuilding the EtherLab configuration. The transition is owned by this
transport, not by a private EtherLab patch:

- it is permitted only while outputs are disarmed;
- it is generation-bound and acknowledged at an exact completed-cycle
  boundary;
- one period governs the whole receive/process/application-time/queue/send
  cycle, and the new period begins with the following cycle;
- it is rejected for DC-configured sessions until application-time, Sync0,
  phase-controller, and user-space timeline changes have one coherent
  transition contract; and
- user space must re-check slave/domain health and timing after the change.

This does not replace EtherLab's optional master send-interval hint. The target
DKMS 1.6.9 build does not export that API, and this project must not carry an
EtherLab fork merely to obtain it.

---

# 29. Migration / fallback strategy

Maintain two backends for a useful period:

```text
CLOCKWORK EtherCAT backend:
    direct
    kernel
```

Selection might be:

```text
--ethercat-backend direct
--ethercat-backend kernel
```

or config-file based.

The old path must remain usable while the new architecture is being validated.

Do not keep duplicated logic forever, but use the fallback during migration.

Aim to share:

```text
device matching
XML parsing
Clockwork IO semantics
```

between both backends.

Only the transport/configuration execution path should differ.

---

# 30. Suggested commit sequence

A good sequence would be approximately:

## Commit 1
Documentation of existing EtherCAT call flow and build environment.

## Commit 2
Minimal external EtherLab kernel application module experiment.

## Commit 3
Character device plus API version/status.

## Commit 4
Slave discovery API and `cw_ec_bus`.

## Commit 5
Generic typed SDO test API and `cw_ec_sdo`.

## Commit 6
ED3L PDO recipe test reproducing `sdo.sh`.

## Commit 7
Generic configuration object model and validation.

## Commit 8
Sync/PDO/PDO-entry application and standalone config tool.

## Commit 9
Domain registration result/entry-ID mapping.

## Commit 10
Kernel cyclic thread and statistics.

## Commit 11
Shared process image plus `cw_ec_io`.

## Commit 12
Common user-space library.

## Commit 13
IOD kernel discovery backend.

## Commit 14
IOD sends existing non-XML PDO configs through kernel.

## Commit 15
IOD sends customised XML-derived PDO configs through kernel.

## Commit 16
IOD process-image integration.

## Commit 17
ED3L setup recipe integrated into IOD startup flow.

## Commit 18
Runtime SDO request mechanism.

## Commit 19
IOD `SDOEntry` migration.

## Commit 20
Distributed clock migration if required.

## Commit 21
Failure/recovery/heartbeat hardening.

## Commit 22
Production startup-script migration.

This is only a guide.
Do not force unrelated changes into arbitrary commit boundaries.

---

# 31. Key design questions Codex must answer during implementation

Do not paper over these questions.

## Master ownership

Exactly when does the module request the EtherLab master?

Possible choices:

```text
module load
first device open
explicit CLAIM_MASTER ioctl
CONFIG_BEGIN
```

Prefer explicit lifecycle control unless EtherLab constraints indicate otherwise.

Need to preserve access for diagnostic `ethercat` CLI when the kernel application is inactive.

## Slave discovery before application ownership

Confirm which EtherLab kernel API is available to retrieve scanned slave identities and when.

## Pre-activation SDO mechanism

Determine the best supported EtherLab kernel API for synchronous setup SDO download before cyclic activation.

Do not blindly implement setup writes using the async runtime request API if a better configuration/startup API exists.

The ordered ED3L mapping sequence must be verifiably completed before activation.

## PDO configuration

Confirm lifetimes required for arrays passed to:

```text
ecrt_slave_config_pdos
```

Do the arrays need to remain allocated for the lifetime of the slave config, or are they copied?

Design memory ownership accordingly.

## Domain registration

Determine exactly how the current code registers entries.

Preserve offset and bit-position semantics.

## mmap lifecycle

What happens to mappings when:

```text
configuration resets
domain size changes
device closes
kernel module unloads
```

Use a generation number and reject stale mappings/configurations.

## Multiple open file descriptors

Decide whether:

- one controller process is allowed;
- diagnostic readers can coexist;
- configuration ownership is exclusive.

A good model may be:

```text
one control owner
multiple read-only status clients
```

but implement only what is necessary initially.

## Permissions

IOD needs access.
Normal unprivileged users should not necessarily be able to command machine outputs or rewrite servo PDOs.

## IOD restart

If IOD restarts while kernel EtherCAT remains active:

- should it reattach to the existing config?
- should kernel deactivate when controlling fd closes?
- should a heartbeat force safe behaviour?

For the first version, cleanly deactivating on controller disconnect may be simpler and safer.

Investigate.

---

# 32. Safety-related constraints

This project controls industrial machinery.

The kernel transport must not introduce hidden output behaviour.

Explicitly document and test:

```text
initial output image
output state before first IOD update
behaviour on IOD crash
behaviour on kernel cycle overrun
behaviour on EtherCAT link loss
behaviour on slave loss
behaviour on configuration mismatch
behaviour during deactivate
behaviour during IOD restart
```

Do not assume setting outputs to zero is always the correct safe action.

Hardware safety systems remain responsible for personnel safety.

Software failure behaviour must nevertheless be deterministic and understood.

---

# 33. Definition of the first useful prototype

The first useful end-to-end prototype does **not** need full Clockwork support.

It is complete when all of these are true:

1. `cw_ethercat.ko` can be loaded and unloaded cleanly.
2. `cw_ec_bus` reports the real bus.
3. `cw_ec_sdo` can reproduce the ED3L `sdo.sh` mapping.
4. `cw_ec_config` can configure a known slave/domain.
5. `cw_ec_cycle` starts a 2 kHz kernel cyclic loop.
6. `cw_ec_io` can read one real input and drive one test output.
7. Cycle statistics can be collected.
8. The old IOD path remains untouched and runnable.

At this point stop and review the architecture before modifying IOD.

This checkpoint is important.

---

# 34. Definition of the second useful prototype

The second prototype is complete when:

1. IOD uses the kernel for bus discovery.
2. IOD's existing Clockwork module matching still works.
3. Customised Beckhoff XML is still parsed in user space.
4. XML-derived PDO definitions are submitted to the kernel.
5. Kernel returns entry offsets/bit positions.
6. Existing `IOComponent` mapping can use those results.
7. Kernel performs cyclic EtherCAT exchange.
8. IOD reads/writes shared process data.
9. Existing ED3L `sdo.sh` can still be used temporarily.

Stop and test extensively here.

---

# 35. Definition of the production migration

Production migration is complete when:

1. ED3L PDO setup recipes are selected by Clockwork and executed through the kernel.
2. `iod.sh` no longer performs servo-specific EtherCAT CLI setup.
3. Runtime SDOEntry functionality works through the kernel backend.
4. Distributed clocks work if production uses them.
5. IOD restart/crash behaviour is defined.
6. Link/slave loss behaviour is defined.
7. Cycle timing is measured and acceptable.
8. Old direct backend remains available for rollback during an agreed transition period.
9. The system has been soak-tested on real machinery.

---

# 36. Recommended initial Codex task

For the first Codex session, do **not** ask it to implement the entire plan.

Give it this narrower task:

> Inspect the `prod-experimental-mqtt-fix` branch and the installed EtherLab kernel/master build. Document the existing Clockwork EtherCAT lifecycle and determine whether an independently built out-of-tree kernel module can call the required EtherLab `ecrt_*` kernel API. Then create a minimal experimental kernel module that requests master 0, reports success/failure, releases it, and unloads cleanly. Add build instructions and a standalone test script. Do not modify IOD behaviour yet.

Expected output:

```text
docs/ethercat-kernel/current-architecture.md
experiments/ethercat_kernel/...
tools/cw_ec_test_master.sh
```

Only after this experiment succeeds should the next Codex task begin bus discovery and the UAPI.

---

# 37. Recommended second Codex task

> Extend the experimental kernel module with a versioned character-device API capable of returning master status and scanned slave identity information. Build a dependency-light `cw_ec_bus` utility that prints the topology. Compare its output to `ethercat master` and `ethercat slaves`. Do not add cyclic operation, PDO configuration, SDO configuration, or IOD integration yet.

This isolates the first real boundary:

```text
EtherLab kernel API
        |
cw_ethercat.ko
        |
versioned UAPI
        |
standalone user-space tool
```

---

# 38. Recommended third Codex task

> Add a generic ordered typed pre-activation SDO-write API and standalone test utility. Reproduce the ED3L PDO-mapping sequence currently implemented by `sdo.sh`, without putting ED3L-specific logic in the kernel. Verify the resulting CoE objects by reading them back. Do not integrate with IOD yet.

This proves the most uncertain configuration requirement before the Clockwork refactor starts.

---

# 39. Recommended fourth Codex task

> Add generic slave/Sync Manager/PDO/PDO-entry configuration submission, validation, domain registration, and mapping-result reporting. Build a standalone configuration tool using a simple test configuration. Do not add mmap or the cyclic kernel thread until configuration can be repeatedly created and torn down cleanly.

---

# 40. Recommended fifth Codex task

> Add the kernel cyclic loop, process-image sharing, timing statistics, and standalone IO tools. Prove real input/output operation and benchmark cycle timing. Do not modify IOD until this standalone path has been demonstrated to be stable.

---

# 41. Final architectural principle

The long-term boundary should remain:

```text
Clockwork decides WHAT the EtherCAT network means and HOW it should be configured.

The kernel module performs the EtherCAT mechanisms and deterministic cyclic exchange.
```

Specifically:

```text
Clockwork owns:
    device identity policy
    module matching
    XML parsing
    PDO choice
    setup-recipe choice
    application IO semantics
    control logic

Kernel owns:
    EtherLab application context
    master lifecycle
    raw scan results
    configuration execution
    ordered setup SDO execution
    domain mapping
    cyclic transport
    process-image exchange
    low-level runtime mailbox execution
```

Preserve this line unless a concrete technical constraint forces it to change.

If such a constraint is found, document it before moving policy into the kernel.

**The kernel module shall own deterministic EtherCAT transport and timing. Higher-level control algorithms—including future motion control, trajectory planning and CiA 402 CSP—should remain layered above the transport unless a demonstrated technical requirement justifies moving functionality into the kernel. The initial UAPI shall therefore expose sufficient timing, generation and cycle information to support future deterministic applications without embedding servo-specific behaviour into the generic transport layer.**
