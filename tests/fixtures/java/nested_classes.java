package com.example.structures;

import java.util.ArrayList;
import java.util.List;

public class OuterContainer {
    private String id;
    private ContainerStatus status;

    public enum ContainerStatus {
        EMPTY,
        ACTIVE,
        FULL
    }

    public interface StateChangeListener {
        void onStateChanged(ContainerStatus oldStatus, ContainerStatus newStatus);
    }

    public static class Configuration {
        private final int maxCapacity;
        private final boolean allowOverflow;

        public Configuration(int maxCapacity, boolean allowOverflow) {
            this.maxCapacity = maxCapacity;
            this.allowOverflow = allowOverflow;
        }

        public int getMaxCapacity() {
            return this.maxCapacity;
        }

        public boolean isAllowOverflow() {
            return this.allowOverflow;
        }
    }

    public class InnerWorker {
        private int processedCount;

        public InnerWorker() {
            this.processedCount = 0;
        }

        public void performWork(String item) {
            if (item != null) {
                this.processedCount++;
            }
        }

        public int getProcessedCount() {
            return this.processedCount;
        }
    }

    public OuterContainer(String id) {
        this.id = id;
        this.status = ContainerStatus.EMPTY;
    }

    public void processWithLocalClass(List<String> items) {
        class LocalProcessor {
            public int countItems(List<String> list) {
                return list.size();
            }
        }

        LocalProcessor processor = new LocalProcessor();
        int total = processor.countItems(items);
        if (total > 0) {
            this.status = ContainerStatus.ACTIVE;
        }
    }

    public void registerListener(StateChangeListener listener) {
        if (listener != null) {
            listener.onStateChanged(ContainerStatus.EMPTY, this.status);
        }
    }

    public void execute() {
        Configuration config = new Configuration(100, false);
        InnerWorker worker = new InnerWorker();
        worker.performWork("payload");

        registerListener(new StateChangeListener() {
            @Override
            public void onStateChanged(ContainerStatus oldStatus, ContainerStatus newStatus) {
                System.out.println(newStatus);
            }
        });
    }
}
