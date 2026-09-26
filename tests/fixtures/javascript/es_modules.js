import defaultParser from './parser';
import * as utils from './utils';
import { readFile, writeFile as writeData } from 'fs';

export const DEFAULT_TIMEOUT = 5000;
export let isRunning = false;

export function parseSource(source, options = {}) {
    return defaultParser(source, options);
}

export class TaskRunner {
    constructor(name) {
        this.name = name;
    }

    run() {
        return utils.execute(this.name);
    }
}

const internalHelper = function() {
    return 42;
};

export default function startService() {
    isRunning = true;
    return internalHelper();
}

export { internalHelper as helperFn };
export * from './constants';
