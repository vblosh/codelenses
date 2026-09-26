import React from "react";
import { UserAccount, UserRole } from "../models";

export interface UserBadgeProps {
    user: UserAccount;
    onSelect?: (user: UserAccount) => void;
}

export const UserBadge: React.FC<UserBadgeProps> = ({ user, onSelect }) => {
    const handleClick = () => {
        if (onSelect) {
            onSelect(user);
        }
    };

    return (
        <div className={`user-badge role-${user.role}`} onClick={handleClick}>
            <span className="username">{user.username}</span>
            <span className="badge-role">{user.role}</span>
        </div>
    );
};

export const UserDirectory: React.FC<{ users: UserAccount[] }> = ({ users }) => {
    return (
        <div className="user-directory">
            <h2>Active Directory</h2>
            <div className="badge-grid">
                {users.map(u => (
                    <UserBadge key={u.id} user={u} />
                ))}
            </div>
        </div>
    );
};
