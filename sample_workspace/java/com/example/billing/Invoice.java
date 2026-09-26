package com.example.billing;

import java.util.ArrayList;
import java.util.List;

public class Invoice implements Billable {
    private final String reference;
    private final List<Double> lineItems;
    private InvoiceStatus status;
    private double taxRate;

    public Invoice(String reference, double taxRate) {
        this.reference = reference;
        this.taxRate = taxRate;
        this.status = InvoiceStatus.DRAFT;
        this.lineItems = new ArrayList<>();
    }

    public void addLineItem(double amount) {
        if (amount > 0.0) {
            this.lineItems.add(amount);
        }
    }

    @Override
    public String getReference() {
        return this.reference;
    }

    @Override
    public InvoiceStatus getStatus() {
        return this.status;
    }

    @Override
    public double calculateTotal() {
        double subtotal = 0.0;
        for (Double item : lineItems) {
            subtotal += item;
        }
        return subtotal * (1.0 + taxRate);
    }

    public void markIssued() {
        if (this.status == InvoiceStatus.DRAFT) {
            this.status = InvoiceStatus.ISSUED;
        }
    }

    public void markPaid() {
        if (this.status == InvoiceStatus.ISSUED) {
            this.status = InvoiceStatus.PAID;
        }
    }
}
