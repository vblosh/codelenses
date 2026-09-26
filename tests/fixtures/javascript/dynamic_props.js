function Animal(name) {
    this.name = name;
}

Animal.prototype.speak = function(sound) {
    return this.name + ' says ' + sound;
};

Animal.prototype.category = 'living';

const config = {};
Object.defineProperty(config, 'maxRetries', {
    value: 3,
    writable: false
});

function accessProperties(obj, dynamicKey) {
    const direct = obj["staticField"];
    const indirect = obj[dynamicKey];
    return direct + indirect;
}
