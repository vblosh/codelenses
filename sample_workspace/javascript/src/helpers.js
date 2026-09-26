/**
 * String and utility formatting helpers.
 */

export function truncate(text, maxLength = 30) {
    if (!text || text.length <= maxLength) {
        return text || "";
    }
    return text.substring(0, maxLength) + "...";
}

export function debounce(func, waitMs) {
    let timeoutId = null;
    return function (...args) {
        if (timeoutId !== null) {
            clearTimeout(timeoutId);
        }
        timeoutId = setTimeout(() => {
            func.apply(this, args);
        }, waitMs);
    };
}

export class Formatter {
    static formatCurrency(amount, currency = "USD") {
        return new Intl.NumberFormat("en-US", {
            style: "currency",
            currency: currency
        }).format(amount);
    }

    static formatTimestamp(date) {
        return new Date(date).toISOString();
    }
}

if (typeof module !== "undefined" && module.exports) {
    module.exports = {
        truncate,
        debounce,
        Formatter
    };
}
