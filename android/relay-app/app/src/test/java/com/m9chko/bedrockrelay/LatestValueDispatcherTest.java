package com.m9chko.bedrockrelay;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;

public final class LatestValueDispatcherTest {
    @Test
    public void slowUiReceivesOnlyNewestSnapshot() {
        ArrayDeque<Runnable> queue = new ArrayDeque<>();
        List<Integer> received = new ArrayList<>();
        LatestValueDispatcher<Integer> dispatcher =
            new LatestValueDispatcher<>(queue::add, received::add);
        for (int i = 0; i < 100_000; ++i) dispatcher.offer(i);
        assertEquals(1, queue.size());
        queue.remove().run();
        assertEquals(List.of(99_999), received);
        assertTrue(queue.isEmpty());
        dispatcher.offer(100_000);
        queue.remove().run();
        assertEquals(List.of(99_999, 100_000), received);
    }

    @Test
    public void snapshotArrivingDuringDeliveryIsNotLost() {
        ArrayDeque<Runnable> queue = new ArrayDeque<>();
        List<Integer> received = new ArrayList<>();
        AtomicReference<LatestValueDispatcher<Integer>> ref = new AtomicReference<>();
        ref.set(new LatestValueDispatcher<>(queue::add, value -> {
            received.add(value);
            if (value == 1) {
                ref.get().offer(2);
                ref.get().offer(3);
            }
        }));
        ref.get().offer(1);
        queue.remove().run();
        assertEquals(1, queue.size());
        queue.remove().run();
        assertEquals(List.of(1, 3), received);
        assertTrue(queue.isEmpty());
    }
}
