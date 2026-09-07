package com.m9chko.bedrockrelay;

import java.util.Objects;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;

/** At most one queued delivery; stale UI snapshots are replaced, not replayed. */
final class LatestValueDispatcher<T> {
    private final Executor executor;
    private final Consumer<T> consumer;
    private final AtomicReference<T> latest = new AtomicReference<>();
    private final AtomicBoolean scheduled = new AtomicBoolean();

    LatestValueDispatcher(Executor executor, Consumer<T> consumer) {
        this.executor = executor;
        this.consumer = consumer;
    }

    void offer(T value) {
        latest.set(Objects.requireNonNull(value));
        schedule();
    }

    private void schedule() {
        if (!scheduled.compareAndSet(false, true)) return;
        try {
            executor.execute(this::deliver);
        } catch (RuntimeException error) {
            scheduled.set(false);
            throw error;
        }
    }

    private void deliver() {
        try {
            T value = latest.getAndSet(null);
            if (value != null) consumer.accept(value);
        } finally {
            scheduled.set(false);
            if (latest.get() != null) schedule();
        }
    }
}
