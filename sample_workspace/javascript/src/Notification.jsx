import React from "react";
import { Formatter, truncate } from "./helpers";

export function NotificationItem({ id, title, message, timestamp, isRead }) {
    return (
        <li className={`notification-item ${isRead ? "read" : "unread"}`}>
            <span className="notif-title">{truncate(title, 25)}</span>
            <p className="notif-body">{message}</p>
            <time className="notif-time">{Formatter.formatTimestamp(timestamp)}</time>
        </li>
    );
}

export function NotificationFeed({ notifications, onClear }) {
    if (!notifications || notifications.length === 0) {
        return <p className="empty-state">No notifications</p>;
    }

    return (
        <div className="notification-feed">
            <header className="feed-header">
                <h3>Notifications ({notifications.length})</h3>
                <button onClick={onClear}>Clear All</button>
            </header>
            <ul className="feed-list">
                {notifications.map(item => (
                    <NotificationItem key={item.id} {...item} />
                ))}
            </ul>
        </div>
    );
}
