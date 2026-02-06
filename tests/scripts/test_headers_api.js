// Test: Headers .get/.has/.set/.delete/.forEach
var resp = await fetch('http://localhost:18080/health');
var headers = resp.headers;

// .has and .get
if (!headers.has('Content-Type')) throw new Error('Expected Content-Type header');
var ct = headers.get('Content-Type');
if (ct !== 'text/plain') throw new Error('Expected text/plain, got ' + ct);

// Case insensitive
if (!headers.has('content-type')) throw new Error('Headers should be case-insensitive');
var ct2 = headers.get('content-type');
if (ct2 !== 'text/plain') throw new Error('Case-insensitive get failed');

// .get for non-existent
if (headers.get('X-Nonexistent') !== null) throw new Error('Expected null for missing header');

// .set
headers.set('X-Test', 'value123');
if (headers.get('X-Test') !== 'value123') throw new Error('set/get failed');

// .delete
headers.delete('X-Test');
if (headers.has('X-Test')) throw new Error('delete failed');

// .forEach
var found = {};
headers.forEach(function(value, name) {
    found[name] = value;
});
if (!found['Content-Type']) throw new Error('forEach did not iterate Content-Type');

console.log('PASS: test_headers_api');
