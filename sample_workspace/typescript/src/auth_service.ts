import { AuthCredentials, Result, SessionToken, UserAccount, UserRole } from "./models";

export class AuthService {
    private users: Map<string, UserAccount> = new Map();
    private activeSessions: Map<string, SessionToken> = new Map();

    public registerUser(user: UserAccount): void {
        this.users.set(user.username, user);
    }

    public authenticate(credentials: AuthCredentials): Result<SessionToken> {
        const user = this.users.get(credentials.username);
        if (!user) {
            return { success: false, error: "User not found" };
        }
        if (!user.isActive) {
            return { success: false, error: "Account inactive" };
        }

        const tokenStr = `token_${user.id}_${Date.now()}`;
        const session: SessionToken = {
            token: tokenStr,
            userId: user.id,
            expiresAt: new Date(Date.now() + 3600 * 1000)
        };

        this.activeSessions.set(tokenStr, session);
        return { success: true, data: session };
    }

    public hasRole(userId: string, requiredRole: UserRole): boolean {
        for (const user of this.users.values()) {
            if (user.id === userId) {
                return user.role === requiredRole;
            }
        }
        return false;
    }
}

export function createGuestAccount(id: string): UserAccount {
    return {
        id,
        username: `guest_${id}`,
        email: `guest_${id}@example.local`,
        role: UserRole.Viewer,
        isActive: true,
        createdAt: new Date()
    };
}
