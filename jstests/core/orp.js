// $or clause deduping with result set sizes > 101 (smaller result sets are now also deduped by the
// query optimizer cursor).

(function() {
'use strict';

const collNamePrefix = 'jstests_orp_';
let collCount = 0;

let t = db.getCollection(collNamePrefix + collCount++);
t.drop();

t.createIndex({a: 1});
t.createIndex({b: 1});
t.createIndex({c: 1});

for (let i = 0; i < 200; ++i) {
    t.save({a: 1, b: 1});
}

// Deduping results from the previous clause.
assert.eq(200, t.count({$or: [{a: 1}, {b: 1}]}));

// Deduping results from a prior clause.
assert.eq(200, t.count({$or: [{a: 1}, {c: 1}, {b: 1}]}));
t.save({c: 1});
assert.eq(201, t.count({$or: [{a: 1}, {c: 1}, {b: 1}]}));

// Deduping results that would normally be index only matches on overlapping and double scanned $or
// field regions.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.createIndex({a: 1, b: 1});
for (let i = 0; i < 16; ++i) {
    for (let j = 0; j < 16; ++j) {
        t.save({a: i, b: j});
    }
}
assert.eq(16 * 16, t.count({$or: [{a: {$gte: 0}, b: {$gte: 0}}, {a: {$lte: 16}, b: {$lte: 16}}]}));

// Deduping results from a clause that completed before the multi cursor takeover.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.createIndex({a: 1});
t.createIndex({b: 1});
t.save({a: 1, b: 200});
for (let i = 0; i < 200; ++i) {
    t.save({b: i});
}
assert.eq(201, t.count({$or: [{a: 1}, {b: {$gte: 0}}]}));
})();
