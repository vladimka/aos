# Design: task and IPC stability

Date: 2026-08-03
Status: approved for specification review

## Goal

Strengthen the current task scheduler and mailbox IPC foundation before expanding
input, storage, GUI, and applications. A QEMU regression scenario must repeatedly
start and end GUI or test processes without a kernel panic, scheduler hang, or lost
process-exit notification.

## Scope

This increment stabilizes existing behavior only. It does not add blocking IPC,
process waiting, mailbox timeouts, task priorities, or new public syscalls. Those
features remain follow-up work after the current scheduler and mailbox behavior is
verified.

## Exit Handling

`task_exit_current()` only marks the current task as exited. `task_switch_kernel()`
is the sole cleanup point for an exited task:

1. Save the exiting task's `sink` and the current `event_pid`.
2. Mark the task `TASK_FREE`.
3. Send one `MSG_TYPE_EXIT` to the saved sink when it names a live, different task.
4. Send one `MSG_TYPE_EXIT` to `event_pid` only when it is a live, nonzero task
   different from both the exited task and sink.
5. Select and restore the next ready task as usual.

This makes exit notifications deterministic and prevents the duplicate delivery
that occurs when both `task_exit_current()` and the scheduler publish the event.

## Mailbox Atomicity

`task_mailbox_send()` protects the full producer operation with its existing
IF-preserving critical section: validating the target's state, computing whether
the ring is full, writing the five-word message, and advancing `mbox_tail`.
`task_mailbox_recv()` similarly protects inspection, copying a message, and
advancing `mbox_head`. The fixed `MSG_CAP` ring and its current return values stay
unchanged:

- `-1`: invalid PID
- `-2`: target task is free
- `-3`: mailbox is full

Exit notification is best-effort: a full mailbox does not retry, block, or delay
task cleanup. Invalid, free, or self-targeted recipients are skipped.

## Test Strategy

Add a host-side QEMU regression harness that boots AOS, drives a sequence of
process launches and exits through the existing task and IPC interfaces, and checks
serial output for the expected single exit notifications. It must also assert that
no `KERNEL PANIC` is emitted and that the window manager remains responsive after
the sequence. The baseline build check is `make`.

## Out of Scope

- Blocking mailbox receive and process wait syscalls
- IPC timeouts or scheduler sleep states
- Scheduler priority policies
- USB-tablet support, filesystem work, and GUI/application features

## Acceptance Criteria

1. `make` completes successfully.
2. Each child task exit produces no more than one notification per distinct valid
   recipient (`sink` and `event_pid`).
3. Concurrent IRQ/syscall mailbox activity cannot observe a partially written
   message or corrupt mailbox indices.
4. The QEMU regression sequence completes without a panic or hang, with the WM
   still responsive.
