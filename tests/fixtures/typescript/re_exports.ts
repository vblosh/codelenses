export { UserService as UserManager } from "./services";
export { Logger } from "./logger";
export * from "./types";
export type { Config } from "./config";

export function init(): void {
    const mgr = new UserManager();
}
