const fs = require('fs');
const { join, resolve: pathResolve } = require('path');
const config = require('./config');

const VERSION = '1.0.0';

function calculateSum(a, b) {
    return a + b;
}

class Logger {
    constructor(prefix) {
        this.prefix = prefix;
    }

    log(message) {
        console.log(this.prefix + ': ' + message);
    }
}

exports.calculateSum = calculateSum;
exports.Logger = Logger;
exports.VERSION = VERSION;
module.exports.formatMessage = function(msg) {
    return String(msg);
};
