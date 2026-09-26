export enum UserRole {
    Admin = "admin",
    Operator = "operator",
    Viewer = "viewer"
}

export interface UserAccount {
    id: string;
    username: string;
    email: string;
    role: UserRole;
    isActive: boolean;
    createdAt: Date;
}

export interface SessionToken {
    token: string;
    userId: string;
    expiresAt: Date;
}

export type Result<T> =
    | { success: true; data: T }
    | { success: false; error: string };

export interface AuthCredentials {
    username: string;
    passwordHash: string;
}
