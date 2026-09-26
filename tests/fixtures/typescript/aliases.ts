import { readFile as read, writeFile as write } from "fs";
import * as path from "path";

export type ID = string | number;
export type Handler<T> = (item: T) => boolean;

export function processFile(filePath: string): boolean {
    const data = read(filePath);
    return write(filePath, data);
}
