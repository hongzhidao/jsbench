// Test: .json() parsing
var resp = await fetch('http://localhost:18080/json');
if (resp.status !== 200) throw new Error('Expected 200, got ' + resp.status);
var data = await resp.json();
if (data.message !== 'hello') throw new Error('Expected message "hello", got "' + data.message + '"');
if (data.number !== 42) throw new Error('Expected number 42, got ' + data.number);
console.log('PASS: test_json');
