export interface IRepository<T, K extends string | number> {
    findById(id: K): T | undefined;
    save(item: T): void;
}

export class MemoryRepository<T, K extends string | number> implements IRepository<T, K> {
    private items: Map<K, T> = new Map();

    findById(id: K): T | undefined {
        return this.items.get(id);
    }

    save(item: T): void {
        // save item
    }
}

export function mapItem<T, R>(item: T, fn: (arg: T) => R): R {
    return fn(item);
}
