# 0009. Kernel thread termination

`KThread::FinishTermination` publishes the thread's signaled state and notifies waiters
under the scheduler lock. It closes the terminated thread after releasing that lock. Orchid
finalizes objects synchronously when their last reference closes. A thread's finalizer can wait
for the process's light locks while unregistering the thread and releasing its TLS page.
Holding the scheduler lock across the last close prevents a contended light-lock wait from
suspending the finalizer safely.
