// Test: Chunked transfer encoding
var resp = await fetch('http://localhost:18080/chunked');
if (resp.status !== 200) throw new Error('Expected 200, got ' + resp.status);
var text = await resp.text();
if (text !== 'Hello, chunked world!')
    throw new Error('Chunked body mismatch: "' + text + '"');
console.log('PASS: test_chunked');
