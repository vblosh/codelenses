package com.example.billing;

public interface Billable {
    String getReference();
    double calculateTotal();
    InvoiceStatus getStatus();
}
